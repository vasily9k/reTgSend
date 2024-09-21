#define CONFIG_TELEGRAM_OUTBOX_SIZE 0

#include "reTgSend.h"
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "mbedtls/ssl.h"

#include "esp_log.h"
#include "esp_event.h"

#define API_TELEGRAM_HOST "api.telegram.org"
#define API_TELEGRAM_PORT 443
#define API_TELEGRAM_TIMEOUT_MS 60000
#define API_TELEGRAM_BOT_PATH "/bot" CONFIG_TELEGRAM_TOKEN
#define API_TELEGRAM_SEND_MESSAGE API_TELEGRAM_BOT_PATH "/sendMessage"
#define API_TELEGRAM_TMPL_MESSAGE "{\"chat_id\":%s,\"parse_mode\":\"HTML\",\"disable_notification\":%s,\"text\":\"%s\r\n\r\n<code>%s</code>\"}"
#define API_TELEGRAM_TMPL_TITLE "<b>%s</b>\r\n\r\n%s"
#define API_TELEGRAM_JSON_SIZE 256
#define API_TELEGRAM_HEADER_CTYPE "Content-Type"
#define API_TELEGRAM_HEADER_AJSON "application/json"
#define API_TELEGRAM_FALSE "false"
#define API_TELEGRAM_TRUE "true"

//#define CONFIG_TELEGRAM_INBOX_QUEUE_WAIT 3000

typedef struct
{
  char *message;
  msg_options_t options;
  time_t timestamp;
} tgMessage_t;

typedef struct
{
#if CONFIG_TELEGRAM_OUTBOX_ENABLE
  bool queued;
#endif // CONFIG_TELEGRAM_OUTBOX_ENABLE
#if CONFIG_TELEGRAM_STATIC_MESSAGE_BUFFER
  char message[CONFIG_TELEGRAM_MESSAGE_SIZE];
#else
  char *message;
#endif // CONFIG_TELEGRAM_MESSAGE_SIZE
  msg_options_t options;
  time_t timestamp;
} tgMessageItem_t;

#define TELEGRAM_QUEUE_ITEM_SIZE sizeof(tgMessage_t *)

TaskHandle_t _tgTask;
TaskHandle_t _tgTaskUpdates;
QueueHandle_t _tgQueue = nullptr;
QueueHandle_t *_tgInboxQueue = nullptr;
#if CONFIG_TELEGRAM_OUTBOX_ENABLE
static tgMessageItem_t _tgOutbox[CONFIG_TELEGRAM_OUTBOX_SIZE];
#endif // CONFIG_TELEGRAM_OUTBOX_ENABLE

static const char *logTAG = "TG";
static const char *tgTaskName = "tg_send";
static const char *tgTaskUpdatesName = "tg_updates";
int tgUpdatesOffset = -3;
int update_status = 0;
bool maxContentSizeExceeded = false;
const cJSON *res_elem = NULL;

#ifndef CONFIG_TELEGRAM_TLS_PEM_STORAGE
#define CONFIG_TELEGRAM_TLS_PEM_STORAGE TLS_CERT_BUFFER
#endif // CONFIG_TELEGRAM_TLS_PEM_STORAGE

#if (CONFIG_TELEGRAM_TLS_PEM_STORAGE == TLS_CERT_BUFFER)
extern const char api_telegram_org_pem_start[] asm(CONFIG_TELEGRAM_TLS_PEM_START);
extern const char api_telegram_org_pem_end[] asm(CONFIG_TELEGRAM_TLS_PEM_END);
#endif // CONFIG_TELEGRAM_TLS_PEM_STORAGE

#if CONFIG_TELEGRAM_STATIC_ALLOCATION
StaticQueue_t _tgQueueBuffer;
StaticTask_t _tgTaskBuffer;
StackType_t _tgTaskStack[CONFIG_TELEGRAM_STACK_SIZE];
uint8_t _tgQueueStorage[CONFIG_TELEGRAM_QUEUE_SIZE * TELEGRAM_QUEUE_ITEM_SIZE];
#endif // CONFIG_TELEGRAM_STATIC_ALLOCATION

bool tgReceiveMsg(cJSON *message, int update_id);

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
  static char *output_buffer; // Buffer to store response of http request from event handler
  static int output_len;      // Stores number of bytes read
  switch (evt->event_id)
  {
  case HTTP_EVENT_ERROR:
    // rlog_d(logTAG, "HTTP_EVENT_ERROR");
    break;
  case HTTP_EVENT_ON_CONNECTED:
    // rlog_d(logTAG, "HTTP_EVENT_ON_CONNECTED");
    break;
  case HTTP_EVENT_HEADER_SENT:
    // rlog_d(logTAG, "HTTP_EVENT_HEADER_SENT");
    break;
  case HTTP_EVENT_ON_HEADER:
    // rlog_d(logTAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
    break;
  case HTTP_EVENT_ON_DATA:
    /*
     *  Check for chunked encoding is added as the URL for chunked encoding used in this example returns binary data.
     *  However, event handler can also be used in case chunked encoding is used.
     */
    if (!esp_http_client_is_chunked_response(evt->client))
    {
      int content_length = esp_http_client_get_content_length(evt->client);
      const int buffer_len = content_length + 1;
      if (output_buffer == NULL)
      {
        if (content_length > 2000)
        {
          if (!maxContentSizeExceeded)
          {
            rlog_d(logTAG, "Content_length > 2000: %d. Current tgUpdatesOffset: %d", content_length, tgUpdatesOffset);
            if (evt->data_len > 46)
            {
              // Получаем номер апдейта(update_id) из первой порции данных.
              // Пользуемся тем, что ответ обычно примерно такой:
              // {"ok":true,"result":[{"update_id":39870880, ...
              // и update_id всегда начинается с 34-го символа
              char str[13];
              memcpy(str, evt->data + 34, 12);
              str[12] = 0;
              int update_id = atoi(str);
              if (update_id)
              {
                tgUpdatesOffset = update_id + 1;
              }
              else
              {
                // Если что-то пошло не так, сбрасываем счетчик апдейтов для получения в следующем запросе последнего апдейта
                tgUpdatesOffset = -1;
              }
              rlog_d(logTAG, "\nСледующий апдейт айди будет:%d\n", tgUpdatesOffset);
            }
          }
          maxContentSizeExceeded = true;
          return ESP_FAIL;
        }
        output_buffer = (char *)malloc(buffer_len);
        output_len = 0;
        if (output_buffer == NULL)
        {
          rlog_e(logTAG, "Failed to allocate memory for output buffer");
          return ESP_FAIL;
        }
      }

      int copy_len = (((evt->data_len) < ((buffer_len - output_len))) ? (evt->data_len) : ((buffer_len - output_len)));
      if (copy_len)
      {
        memcpy(output_buffer + output_len, evt->data, copy_len);
      }
      output_len += copy_len;
      //rlog_d(logTAG, "HTTP_EVENT_ON_DATA, получено=%d, copy_len=%d, output_len=%d", evt->data_len, copy_len, output_len);
    }
    break;
  case HTTP_EVENT_ON_FINISH:
    rlog_d(logTAG, "HTTP_EVENT_ON_FINISH");
    update_status = 0;
    if (output_buffer != NULL)
    {
      output_buffer[output_len] = 0;
      rlog_d(logTAG, "output_len=%d. Update:\n%s", output_len, output_buffer);

      cJSON *resp = cJSON_Parse(output_buffer);
      cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
      cJSON_ArrayForEach(res_elem, result)
      {
        cJSON *update_id = cJSON_GetObjectItemCaseSensitive(res_elem, "update_id");
        if (cJSON_IsNumber(update_id))
        {
          update_status = update_id->valuedouble;
          tgUpdatesOffset = update_status + 1;
          cJSON *message = cJSON_GetObjectItemCaseSensitive(res_elem, "message");
          if (cJSON_HasObjectItem(message, "text") || cJSON_HasObjectItem(message, "document"))
          {
            if (tgReceiveMsg(message, update_status))
            {
              rlog_d(logTAG, "incoming message queued");
            }
            else
            {
              rlog_d(logTAG, "incoming message not queued");
            }
          }
        }
      }
      if (update_status == 0)
      {
        rlog_d(logTAG, "No updates");
      }

      cJSON_Delete(resp);
      free(output_buffer);
      output_buffer = NULL;
    }
    output_len = 0;
    maxContentSizeExceeded = false;
    break;
  case HTTP_EVENT_DISCONNECTED:
    rlog_d(logTAG, "HTTP_EVENT_DISCONNECTED");
    if (output_buffer != NULL)
    {
      free(output_buffer);
      output_buffer = NULL;
    }
    maxContentSizeExceeded = false;
    output_len = 0;
    break;
  case HTTP_EVENT_REDIRECT:
    rlog_d(logTAG, "HTTP_EVENT_REDIRECT");
    //esp_http_client_set_header(evt->client, "From", "user@example.com");
    //esp_http_client_set_header(evt->client, "Accept", "text/html");
    //esp_http_client_set_redirection(evt->client);
    break;
  }
  return ESP_OK;
}

esp_err_t _getFile_response_event_handler(esp_http_client_event_t *evt)
{
  static char *output_buffer; // Buffer to store response of http request from event handler
  static int output_len;      // Stores number of bytes read
  static bool maxContentSizeExceeded = false;
  switch (evt->event_id)
  {
  case HTTP_EVENT_ERROR:
  case HTTP_EVENT_ON_CONNECTED:
  case HTTP_EVENT_HEADER_SENT:
  case HTTP_EVENT_ON_HEADER:
    break;
  case HTTP_EVENT_ON_DATA:
    if (!esp_http_client_is_chunked_response(evt->client))
    {
      int content_length = esp_http_client_get_content_length(evt->client);
      const int buffer_len = content_length + 1;
      if (output_buffer == NULL)
      {
        if (content_length > 600)
        {
          if (!maxContentSizeExceeded)
          {
            rlog_d(logTAG, "Content_length > 600: %d.", content_length);
          }
          maxContentSizeExceeded = true;
          return ESP_FAIL;
        }
        output_buffer = (char *)malloc(buffer_len);
        output_len = 0;
        if (output_buffer == NULL)
        {
          rlog_e(logTAG, "Failed to allocate memory for output buffer");
          return ESP_FAIL;
        }
      }

      int copy_len = (((evt->data_len) < ((buffer_len - output_len))) ? (evt->data_len) : ((buffer_len - output_len)));
      if (copy_len)
      {
        memcpy(output_buffer + output_len, evt->data, copy_len);
      }
      output_len += copy_len;
      rlog_d(logTAG, "HTTP_EVENT_ON_DATA, получено=%d, copy_len=%d, output_len=%d", evt->data_len, copy_len, output_len);
    }
    break;
  case HTTP_EVENT_ON_FINISH:
    rlog_d(logTAG, "HTTP_EVENT_ON_FINISH");
    if (output_buffer != NULL)
    {
      output_buffer[output_len] = 0;
      rlog_d(logTAG, "output_len=%d. Body:\n%s", output_len, output_buffer);

      cJSON *resp = cJSON_Parse(output_buffer);
      cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
      if (cJSON_HasObjectItem(result, "file_path"))
      {
        if (tgSendLinkToFile(result))
        {
          rlog_d(logTAG, "incoming LinkToFile queued");
        }
        else
        {
          rlog_d(logTAG, "incoming LinkToFile not queued");
        }
      }
      cJSON_Delete(resp);
      free(output_buffer);
      output_buffer = NULL;
    }
    output_len = 0;
    maxContentSizeExceeded = false;
    break;
  case HTTP_EVENT_DISCONNECTED:
    rlog_d(logTAG, "HTTP_EVENT_DISCONNECTED");
    if (output_buffer != NULL)
    {
      free(output_buffer);
      output_buffer = NULL;
    }
    maxContentSizeExceeded = false;
    output_len = 0;
    break;
  case HTTP_EVENT_REDIRECT:
    rlog_d(logTAG, "HTTP_EVENT_REDIRECT");
    break;
  }
  return ESP_OK;
}

char *tgNotifyApi(tgMessageItem_t *tgMsg)
{
  if (decMsgOptionsNotify(tgMsg->options))
    return (char *)API_TELEGRAM_FALSE;
  else
    return (char *)API_TELEGRAM_TRUE;
}

esp_err_t tgSendApi(tgMessageItem_t *tgMsg)
{
  rlog_i(logTAG, "Send message: %s", tgMsg->message);

  struct tm timeinfo;
  static char buffer_timestamp[CONFIG_BUFFER_LEN_INT64_RADIX10];
#if CONFIG_TELEGRAM_STATIC_MESSAGE_BUFFER
  static char buffer_json[CONFIG_TELEGRAM_MESSAGE_SIZE + API_TELEGRAM_JSON_SIZE];
#else
  char *buffer_json = nullptr;
#endif // CONFIG_TELEGRAM_MESSAGE_SIZE

  // Determine chat ID
  const char *chat_id;
  switch (decMsgOptionsKind(tgMsg->options))
  {
  case MK_SERVICE:
#ifdef CONFIG_TELEGRAM_CHAT_ID_SERVICE
    chat_id = CONFIG_TELEGRAM_CHAT_ID_SERVICE;
#else
    chat_id = CONFIG_TELEGRAM_CHAT_ID_MAIN;
#endif // CONFIG_TELEGRAM_CHAT_ID_SERVICE
    break;

  case MK_PARAMS:
#ifdef CONFIG_TELEGRAM_CHAT_ID_PARAMS
    chat_id = CONFIG_TELEGRAM_CHAT_ID_PARAMS;
#else
    chat_id = CONFIG_TELEGRAM_CHAT_ID_MAIN;
#endif // CONFIG_TELEGRAM_CHAT_ID_PARAMS
    break;

  case MK_SECURITY:
#ifdef CONFIG_TELEGRAM_CHAT_ID_SECURITY
    chat_id = CONFIG_TELEGRAM_CHAT_ID_SECURITY;
#else
    chat_id = CONFIG_TELEGRAM_CHAT_ID_MAIN;
#endif // CONFIG_TELEGRAM_CHAT_ID_SECURITY
    break;

  default:
    chat_id = CONFIG_TELEGRAM_CHAT_ID_MAIN;
    break;
  };
  if (strcmp(chat_id, "") == 0)
  {
    rlog_d(logTAG, "Chat ID not set, message ignored");
    return ESP_OK;
  };

  // Preparing JSON to send
  localtime_r(&tgMsg->timestamp, &timeinfo);
  strftime(buffer_timestamp, sizeof(buffer_timestamp), CONFIG_FORMAT_DTS, &timeinfo);
#if CONFIG_TELEGRAM_STATIC_MESSAGE_BUFFER
  uint16_t size = format_string(buffer_json, CONFIG_TELEGRAM_MESSAGE_SIZE + API_TELEGRAM_JSON_SIZE, API_TELEGRAM_TMPL_MESSAGE,
                                chat_id, tgNotifyApi(tgMsg), tgMsg->message, buffer_timestamp);
  if (size == 0)
  {
    rlog_e(logTAG, "Failed to create json request to Telegram API");
    return ESP_ERR_NO_MEM;
  };
#else
  buffer_json = malloc_stringf(API_TELEGRAM_TMPL_MESSAGE,
                               chat_id, tgNotifyApi(tgMsg), tgMsg->message, buffer_timestamp);
  if (buffer_json == nullptr)
  {
    rlog_e(logTAG, "Failed to create json request to Telegram API");
    return ESP_ERR_NO_MEM;
  };
#endif // CONFIG_TELEGRAM_MESSAGE_SIZE

  // Configuring request parameters
  esp_err_t ret = ESP_FAIL;
  esp_http_client_config_t cfgHttp;
  memset(&cfgHttp, 0, sizeof(cfgHttp));
  cfgHttp.method = HTTP_METHOD_POST;
  cfgHttp.host = API_TELEGRAM_HOST;
  cfgHttp.port = API_TELEGRAM_PORT;
  cfgHttp.path = API_TELEGRAM_SEND_MESSAGE;
  cfgHttp.timeout_ms = API_TELEGRAM_TIMEOUT_MS;
  cfgHttp.transport_type = HTTP_TRANSPORT_OVER_SSL;
#if CONFIG_TELEGRAM_TLS_PEM_STORAGE == TLS_CERT_BUFFER
  cfgHttp.cert_pem = api_telegram_org_pem_start;
  cfgHttp.use_global_ca_store = false;
#elif CONFIG_TELEGRAM_TLS_PEM_STORAGE == TLS_CERT_GLOBAL
  cfgHttp.use_global_ca_store = true;
#elif CONFIG_TELEGRAM_TLS_PEM_STORAGE == TLS_CERT_BUNGLE
  cfgHttp.crt_bundle_attach = esp_crt_bundle_attach;
  cfgHttp.use_global_ca_store = false;
#endif // CONFIG_TELEGRAM_TLS_PEM_STORAGE
  cfgHttp.skip_cert_common_name_check = false;
  cfgHttp.is_async = false;

  // Make request to Telegram API
  esp_http_client_handle_t client = esp_http_client_init(&cfgHttp);
  if (client)
  {
    esp_http_client_set_header(client, API_TELEGRAM_HEADER_CTYPE, API_TELEGRAM_HEADER_AJSON);
    esp_http_client_set_post_field(client, buffer_json, strlen(buffer_json));
    ret = esp_http_client_perform(client);
    if (ret == ESP_OK)
    {
      int retCode = esp_http_client_get_status_code(client);
      if (retCode == HttpStatus_Ok)
      {
        ret = ESP_OK;
        rlog_v(logTAG, "Message sent: %s", tgMsg->message);
      }
      else if (retCode == HttpStatus_Forbidden)
      {
        ret = ESP_ERR_INVALID_RESPONSE;
        rlog_w(logTAG, "Failed to send message, too many messages, please wait");
      }
      else
      {
        ret = ESP_ERR_INVALID_ARG;
        rlog_e(logTAG, "Failed to send message, API error code: #%d!", retCode);
      };
#if !defined(CONFIG_TELEGRAM_SYSLED_ACTIVITY) || CONFIG_TELEGRAM_SYSLED_ACTIVITY
      // Flashing system LED
      ledSysActivity();
#endif // CONFIG_TELEGRAM_SYSLED_ACTIVITY
    }
    else
    {
      rlog_e(logTAG, "Failed to complete request to Telegram API, error code: 0x%x!", ret);
    };
    esp_http_client_cleanup(client);
  }
  else
  {
    ret = ESP_ERR_INVALID_STATE;
    rlog_e(logTAG, "Failed to complete request to Telegram API!");
  };

// Free buffer
#if !CONFIG_TELEGRAM_STATIC_MESSAGE_BUFFER
  if (buffer_json != nullptr)
    free(buffer_json);
#endif // CONFIG_TELEGRAM_MESSAGE_SIZE

  return ret;
}

bool tgSendMsg(msg_options_t msgOptions, const char *msgTitle, const char *msgText, ...)
{
  if (_tgQueue)
  {
    tgMessage_t *tgMsg = (tgMessage_t *)esp_calloc(1, sizeof(tgMessage_t));
    if (tgMsg)
    {
      tgMsg->options = msgOptions;
      tgMsg->timestamp = time(nullptr);
      tgMsg->message = nullptr;

      // Allocate memory for the message text and format it
      va_list args;
      va_start(args, msgText);
      uint16_t len = vsnprintf(nullptr, 0, msgText, args);
      tgMsg->message = (char *)esp_calloc(1, len + 1);
      if (tgMsg->message)
      {
        vsnprintf(tgMsg->message, len + 1, msgText, args);
      }
      else
      {
        rlog_e(logTAG, "Failed to allocate memory for message text");
        va_end(args);
        goto error;
      };
      va_end(args);

// Add title if available
#if CONFIG_TELEGRAM_TITLE_ENABLED
      if (msgTitle)
      {
        char *temp_message = tgMsg->message;
        tgMsg->message = malloc_stringf(API_TELEGRAM_TMPL_TITLE, msgTitle, temp_message);
        if (tgMsg->message == nullptr)
        {
          rlog_e(logTAG, "Failed to allocate memory for message text");
          goto error;
        };
        free(temp_message);
      };
#endif // CONFIG_TELEGRAM_TITLE_ENABLED

      // Put a message to the task queue
      if (xQueueSend(_tgQueue, &tgMsg, pdMS_TO_TICKS(CONFIG_TELEGRAM_QUEUE_WAIT)) == pdPASS)
      {
        return true;
      }
      else
      {
        rloga_e("Failed to adding message to queue [ %s ]!", tgTaskName);
        eventLoopPostError(RE_SYS_TELEGRAM_ERROR, ESP_ERR_NO_MEM);
        goto error;
      };
    }
    else
    {
      rlog_e(logTAG, "Failed to allocate memory for message");
    };
  error:
    // Deallocate resources from heap
    if (tgMsg)
    {
      if (tgMsg->message)
        free(tgMsg->message);
      free(tgMsg);
    };
  };
  return false;
}

esp_err_t tgApi()
{
  // Configuring request parameters
  esp_err_t ret = ESP_FAIL;
  esp_http_client_config_t cfgHttp;

  memset(&cfgHttp, 0, sizeof(cfgHttp));
  cfgHttp.method = HTTP_METHOD_GET;
  cfgHttp.host = API_TELEGRAM_HOST;
  cfgHttp.port = API_TELEGRAM_PORT;

  char *res = nullptr;
  res = malloc_stringf("/bot" CONFIG_TELEGRAM_TOKEN "/getUpdates?limit=1&timeout=60&offset=%d", tgUpdatesOffset);
  rlog_d(logTAG, "tgApi request update offset:%d", tgUpdatesOffset);

  cfgHttp.path = res;
  cfgHttp.timeout_ms = API_TELEGRAM_TIMEOUT_MS;
  cfgHttp.transport_type = HTTP_TRANSPORT_OVER_SSL;
#if CONFIG_TELEGRAM_TLS_PEM_STORAGE == TLS_CERT_BUFFER
  cfgHttp.cert_pem = api_telegram_org_pem_start;
  cfgHttp.use_global_ca_store = false;
#elif CONFIG_TELEGRAM_TLS_PEM_STORAGE == TLS_CERT_GLOBAL
  cfgHttp.use_global_ca_store = true;
#elif CONFIG_TELEGRAM_TLS_PEM_STORAGE == TLS_CERT_BUNGLE
  cfgHttp.crt_bundle_attach = esp_crt_bundle_attach;
  cfgHttp.use_global_ca_store = false;
#endif // CONFIG_TELEGRAM_TLS_PEM_STORAGE
  cfgHttp.skip_cert_common_name_check = false;
  cfgHttp.is_async = false;
  cfgHttp.event_handler = _http_event_handler;

  // Make request to Telegram API
  esp_http_client_handle_t client = esp_http_client_init(&cfgHttp);
  if (client)
  {
    esp_http_client_set_header(client, API_TELEGRAM_HEADER_CTYPE, API_TELEGRAM_HEADER_AJSON);
    ret = esp_http_client_perform(client);
    if (ret == ESP_OK)
    {
      int retCode = esp_http_client_get_status_code(client);
      rlog_d(logTAG, "HTTP GET Status = %d, content_length = %" PRId64,
             retCode, esp_http_client_get_content_length(client));
      if (retCode == HttpStatus_Ok)
      {
        ret = ESP_OK;
        rlog_v(logTAG, "HTTP GET request to Telegram API done");
      }
      else if (retCode == HttpStatus_Forbidden)
      {
        ret = ESP_ERR_INVALID_RESPONSE;
        rlog_w(logTAG, "Failed to send message, too many messages, please wait");
      }
      else
      {
        ret = ESP_ERR_INVALID_ARG;
        rlog_e(logTAG, "Failed to send message, API error code: #%d!", retCode);
      };
    }
    else
    {
      rlog_e(logTAG, "HTTP GET request to Telegram API failed: %s", esp_err_to_name(ret));
    };
    esp_http_client_cleanup(client);
  }
  else
  {
    ret = ESP_ERR_INVALID_STATE;
    rlog_e(logTAG, "Failed to complete request to Telegram API!");
  };

  free(res);
  return ret;
}

bool tgReceiveMsg(cJSON *message, int update_id)
{
  if (*_tgInboxQueue)
  {
    tgUpdateMessage_t *tgMsg = (tgUpdateMessage_t *)esp_calloc(1, sizeof(tgUpdateMessage_t));
    if (tgMsg)
    {
      tgMsg->chat_id = 0;
      tgMsg->from_id = 0;
      tgMsg->date = 0;
      tgMsg->update_id = update_id;
      tgMsg->type = TG_MESSAGE_UNKNOWN;
      tgMsg->text = nullptr;
      tgMsg->file = nullptr;

      cJSON *message = cJSON_GetObjectItemCaseSensitive(res_elem, "message");
      if (cJSON_HasObjectItem(message, "document"))
      {
        tgMsg->type = TG_MESSAGE_DOCUMENT;
        tgMsg->file = (tgUpdateDocument_t *)esp_calloc(1, sizeof(tgUpdateDocument_t));
        if (tgMsg->file)
        {
          cJSON *document = cJSON_GetObjectItemCaseSensitive(message, "document");
          cJSON *file_name = cJSON_GetObjectItemCaseSensitive(document, "file_name");
          tgMsg->file->name = (char *)esp_calloc(1, strlen(file_name->valuestring) + 1);

          if (tgMsg->file->name)
          {
            memcpy(tgMsg->file->name, file_name->valuestring, strlen(file_name->valuestring));
          }
          else
          {
            rlog_e(logTAG, "Failed to allocate memory for document name");
            goto error;
          };

          cJSON *file_id = cJSON_GetObjectItemCaseSensitive(document, "file_id");
          tgMsg->file->id = (char *)esp_calloc(1, strlen(file_id->valuestring) + 1);
          if (tgMsg->file->id)
          {
            memcpy(tgMsg->file->id, file_id->valuestring, strlen(file_id->valuestring));
          }
          else
          {
            rlog_e(logTAG, "Failed to allocate memory for document file_id");
            goto error;
          };

          if (cJSON_HasObjectItem(message, "caption"))
          {
            cJSON *caption = cJSON_GetObjectItemCaseSensitive(message, "caption");
            tgMsg->file->caption = (char *)esp_calloc(1, strlen(caption->valuestring) + 1);
            if (tgMsg->file->caption)
            {
              memcpy(tgMsg->file->caption, caption->valuestring, strlen(caption->valuestring));
              rlog_d(logTAG, "tgMsg->file->caption:%s\n", tgMsg->file->caption);
            }
            else
            {
              rlog_e(logTAG, "Failed to allocate memory for message caption");
              goto error;
            };
          }
          else
          {
            tgMsg->file->caption = nullptr;
          }

          cJSON *file_size = cJSON_GetObjectItemCaseSensitive(document, "file_size");
          if (cJSON_IsNumber(file_size))
            tgMsg->file->size = file_size->valuedouble;
        }
        else
        {
          rlog_e(logTAG, "Failed to allocate memory for document file_size");
          goto error;
        }
      }

      if (cJSON_HasObjectItem(message, "text"))
      {
        tgMsg->type = TG_MESSAGE_TEXT;
        cJSON *text = cJSON_GetObjectItemCaseSensitive(message, "text");
        tgMsg->text = (char *)esp_calloc(1, strlen(text->valuestring) + 1);
        if (tgMsg->text)
        {
          memcpy(tgMsg->text, text->valuestring, strlen(text->valuestring));
          rlog_d(logTAG, "tgMsg->text:%s\n", tgMsg->text);
        }
        else
        {
          rlog_e(logTAG, "Failed to allocate memory for message text");
          goto error;
        };
      }

      if (tgMsg->type > TG_MESSAGE_UNKNOWN)
      {
        cJSON *chat = cJSON_GetObjectItemCaseSensitive(message, "chat");
        cJSON *chat_id = cJSON_GetObjectItemCaseSensitive(chat, "id");
        if (cJSON_IsNumber(chat_id))
          tgMsg->chat_id = chat_id->valuedouble;

        cJSON *from = cJSON_GetObjectItemCaseSensitive(message, "from");
        cJSON *from_id = cJSON_GetObjectItemCaseSensitive(from, "id");
        if (cJSON_IsNumber(from_id))
          tgMsg->from_id = from_id->valuedouble;

        cJSON *date = cJSON_GetObjectItemCaseSensitive(message, "date");
        if (cJSON_IsNumber(date))
          tgMsg->date = date->valuedouble;
      }

      if (xQueueSend(*_tgInboxQueue, &tgMsg, pdMS_TO_TICKS(CONFIG_TELEGRAM_INBOX_QUEUE_WAIT)) == pdPASS)
      {
        return true;
      }
      else
      {
        rloga_e("Failed to adding message to inbox queue [ _tgInboxQueue ]!");
        eventLoopPostError(RE_SYS_TELEGRAM_ERROR, ESP_ERR_NO_MEM);
        goto error;
      };
    }
    else
    {
      rlog_e(logTAG, "Failed to allocate memory for message");
    };
  error:
    // Deallocate resources from heap
    if (tgMsg)
    {
      if (tgMsg->text)
        free(tgMsg->text);
      if (tgMsg->file)
      {
        if (tgMsg->file->caption)
          free(tgMsg->file->caption);
        if (tgMsg->file->name)
          free(tgMsg->file->name);
        if (tgMsg->file->id)
          free(tgMsg->file->id);
        free(tgMsg->file);
      };
      free(tgMsg);
    };
  };
  return false;
}

bool tgSendLinkToFile(cJSON *result)
{
  if (*_tgInboxQueue)
  {
    tgUpdateMessage_t *tgMsg = (tgUpdateMessage_t *)esp_calloc(1, sizeof(tgUpdateMessage_t));
    if (tgMsg)
    {
      tgMsg->chat_id = 0;
      tgMsg->from_id = 0;
      tgMsg->date = 0;
      tgMsg->update_id = 0;
      tgMsg->type = TG_MESSAGE_LINKTOFILE;
      tgMsg->file = nullptr;

      cJSON *file_path = cJSON_GetObjectItemCaseSensitive(result, "file_path");

      int file_number = 0;
      if (file_path->valuestring)
      {
        int ret = sscanf(file_path->valuestring, "documents/file_%d", &file_number);
      }

      cJSON *file_size = cJSON_GetObjectItemCaseSensitive(result, "file_size");
      static const char fmt[] = "Получен файл №%d, размером %d байт.\n"
                         "Для обновления прошивки выполните команду\n"
                         "/upgrade_%d";
      int f_sz = file_size->valuedouble ? file_size->valuedouble : 0;

      int sz = snprintf(NULL, 0, fmt, file_number, f_sz, file_number);
      tgMsg->text = (char *)esp_calloc(1, sz + 1);
      snprintf(tgMsg->text, sz + 1, fmt, file_number, f_sz, file_number);

      if (xQueueSend(*_tgInboxQueue, &tgMsg, pdMS_TO_TICKS(CONFIG_TELEGRAM_INBOX_QUEUE_WAIT)) == pdPASS)
      {
        return true;
      }
      else
      {
        rloga_e("Failed to adding message to inbox queue [ _tgInboxQueue ]!");
        eventLoopPostError(RE_SYS_TELEGRAM_ERROR, ESP_ERR_NO_MEM);
        goto error;
      };
    }
    else
    {
      rlog_e(logTAG, "Failed to allocate memory for message");
    };
  error:
    // Deallocate resources from heap
    if (tgMsg)
    {
      if (tgMsg->file)
      {
        if (tgMsg->file->name)
          free(tgMsg->file->name);
        free(tgMsg->file);
      };
      free(tgMsg);
    };
  };
  return false;
}

esp_err_t tgGetFileApi(char *file_id)
{
  esp_err_t ret = ESP_FAIL;
  esp_http_client_config_t cfgHttp;

  memset(&cfgHttp, 0, sizeof(cfgHttp));
  cfgHttp.method = HTTP_METHOD_GET;
  cfgHttp.host = API_TELEGRAM_HOST;
  cfgHttp.port = API_TELEGRAM_PORT;

  char *res = nullptr;
  res = malloc_stringf("/bot" CONFIG_TELEGRAM_TOKEN "/getFile?file_id=%s", file_id);

  cfgHttp.path = res;
  cfgHttp.timeout_ms = 5000;
  cfgHttp.transport_type = HTTP_TRANSPORT_OVER_SSL;
#if CONFIG_TELEGRAM_TLS_PEM_STORAGE == TLS_CERT_BUFFER
  cfgHttp.cert_pem = api_telegram_org_pem_start;
  cfgHttp.use_global_ca_store = false;
#elif CONFIG_TELEGRAM_TLS_PEM_STORAGE == TLS_CERT_GLOBAL
  cfgHttp.use_global_ca_store = true;
#elif CONFIG_TELEGRAM_TLS_PEM_STORAGE == TLS_CERT_BUNGLE
  cfgHttp.crt_bundle_attach = esp_crt_bundle_attach;
  cfgHttp.use_global_ca_store = false;
#endif // CONFIG_TELEGRAM_TLS_PEM_STORAGE
  cfgHttp.skip_cert_common_name_check = false;
  cfgHttp.is_async = false;
  cfgHttp.event_handler = _getFile_response_event_handler;

  // Make request to Telegram API
  esp_http_client_handle_t client = esp_http_client_init(&cfgHttp);
  if (client)
  {
    esp_http_client_set_header(client, API_TELEGRAM_HEADER_CTYPE, API_TELEGRAM_HEADER_AJSON);
    ret = esp_http_client_perform(client);
    if (ret == ESP_OK)
    {
      int retCode = esp_http_client_get_status_code(client);
      rlog_d(logTAG, "HTTP GET Status = %d, content_length = %" PRId64,
             retCode, esp_http_client_get_content_length(client));
      if (retCode == HttpStatus_Ok)
      {
        ret = ESP_OK;
        rlog_v(logTAG, "HTTP GET request to Telegram API done");
      }
      else if (retCode == HttpStatus_Forbidden)
      {
        ret = ESP_ERR_INVALID_RESPONSE;
        rlog_w(logTAG, "Failed to send message, too many messages, please wait");
      }
      else
      {
        ret = ESP_ERR_INVALID_ARG;
        rlog_e(logTAG, "Failed to send message, API error code: #%d!", retCode);
      };
    }
    else
    {
      rlog_e(logTAG, "HTTP GET request to Telegram API failed: %s", esp_err_to_name(ret));
    };
    esp_http_client_cleanup(client);
  }
  else
  {
    ret = ESP_ERR_INVALID_STATE;
    rlog_e(logTAG, "Failed to complete request to Telegram API!");
  };

  free(res);
  return ret;
}

// -----------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------- Task routines ----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

void tgTaskExec(void *pvParameters)
{
  tgMessage_t *inMsg;
  esp_err_t resLast = ESP_OK;

// Init outgoing message queue
#if CONFIG_TELEGRAM_OUTBOX_ENABLE
  static uint8_t _tgOutboxSize = 0;
  TickType_t waitIncoming = portMAX_DELAY;

  rlog_d(logTAG, "Initialize telegram outbox...");
  memset(_tgOutbox, 0, sizeof(tgMessageItem_t) * CONFIG_TELEGRAM_OUTBOX_SIZE);
#endif // CONFIG_TELEGRAM_OUTBOX_ENABLE

  while (true)
  {
// Outbox mode
#if CONFIG_TELEGRAM_OUTBOX_ENABLE
    // Calculate the timeout for an incoming message
    if (_tgOutboxSize > 0)
    {
      if (statesWiFiIsConnected())
      {
        // If the API denied service last time, you'll have to wait longer
        if (resLast == ESP_ERR_INVALID_RESPONSE)
        {
          waitIncoming = pdMS_TO_TICKS(CONFIG_TELEGRAM_FORBIDDEN_INTERVAL);
        }
        else
        {
          waitIncoming = pdMS_TO_TICKS(CONFIG_TELEGRAM_SEND_INTERVAL);
        };
      }
      else
      {
        waitIncoming = pdMS_TO_TICKS(CONFIG_TELEGRAM_INTERNET_INTERVAL);
      };
    }
    else
    {
      waitIncoming = portMAX_DELAY;
    };

    // Waiting for an incoming message
    if (xQueueReceive(_tgQueue, &inMsg, waitIncoming) == pdPASS)
    {
      rlog_d(logTAG, "New message received (outbox size: %d): %s", _tgOutboxSize, inMsg->message);

      // Search for a lower priority message that could be deleted
      if (_tgOutboxSize >= CONFIG_TELEGRAM_OUTBOX_SIZE)
      {
        msg_priority_t inPriority = decMsgOptionsPriority(inMsg->options);
        for (uint8_t i = 0; i < CONFIG_TELEGRAM_OUTBOX_SIZE; i++)
        {
          if (decMsgOptionsPriority(_tgOutbox[i].options) < inPriority)
          {
            rlog_w(logTAG, "Message dropped from send outbox (size: %d, index: %d): %s", _tgOutboxSize, i, _tgOutbox[i].message);
#if !CONFIG_TELEGRAM_STATIC_MESSAGE_BUFFER
            if (_tgOutbox[i].message)
              free(_tgOutbox[i].message);
            _tgOutbox[i].message = nullptr;
#endif // CONFIG_TELEGRAM_MESSAGE_SIZE
            _tgOutbox[i].queued = false;
            _tgOutboxSize--;
            break;
          };
        };
      };

      // Insert new message to outbox
      if (_tgOutboxSize < CONFIG_TELEGRAM_OUTBOX_SIZE)
      {
        for (uint8_t i = 0; i < CONFIG_TELEGRAM_OUTBOX_SIZE; i++)
        {
          if (!_tgOutbox[i].queued)
          {
#if CONFIG_TELEGRAM_STATIC_MESSAGE_BUFFER
            memset(_tgOutbox[i].message, 0, CONFIG_TELEGRAM_MESSAGE_SIZE);
            strncpy(_tgOutbox[i].message, inMsg->message, CONFIG_TELEGRAM_MESSAGE_SIZE - 1);
#else
            _tgOutbox[i].message = inMsg->message;
            inMsg->message = nullptr;
#endif // CONFIG_TELEGRAM_MESSAGE_SIZE
            _tgOutbox[i].queued = true;
            _tgOutbox[i].options = inMsg->options;
            _tgOutbox[i].timestamp = inMsg->timestamp;
            _tgOutboxSize++;
            rlog_d(logTAG, "Message inserted to send outbox (size: %d, index: %d): %s", _tgOutboxSize, i, _tgOutbox[i].message);
            break;
          };
        };
      }
      else
      {
        rlog_e(logTAG, "Failed to insert message to send outbox (outbox size: %d): queue is full", _tgOutboxSize);
      };

      // Delete the resources used for the message
      if (inMsg->message)
        free(inMsg->message);
      free(inMsg);
      inMsg = nullptr;
    };

    // Search for the first message in the outbox
    if (statesWiFiIsConnected())
    {
      _tgOutboxSize = 0;
      uint8_t first_msg = 0xFF;
      time_t first_time = 0;
      for (uint8_t i = 0; i < CONFIG_TELEGRAM_OUTBOX_SIZE; i++)
      {
        if (_tgOutbox[i].queued)
        {
          if ((first_time == 0) || (_tgOutbox[i].timestamp < first_time))
          {
            first_msg = i;
            first_time = _tgOutbox[i].timestamp;
          };
          _tgOutboxSize++;
        };
      };

      // If the queue is not empty, send the first found message
      if ((_tgOutboxSize > 0) && (first_msg < CONFIG_TELEGRAM_OUTBOX_SIZE))
      {
        esp_err_t resSend = tgSendApi(&_tgOutbox[first_msg]);
        // If the send status has changed, send an event to the event loop
        if (resSend != resLast)
        {
          resLast = resSend;
          eventLoopPostError(RE_SYS_TELEGRAM_ERROR, resLast);
        };
        // If the message is sent (ESP_OK) or too long (ESP_ERR_NO_MEM) or an API error (ESP_ERR_INVALID_ARG - bad message?), then remove it from the queue
        if ((resSend == ESP_OK) || (resSend == ESP_ERR_NO_MEM) || (resSend == ESP_ERR_INVALID_ARG))
        {
#if !CONFIG_TELEGRAM_STATIC_MESSAGE_BUFFER
          if (_tgOutbox[first_msg].message)
            free(_tgOutbox[first_msg].message);
          _tgOutbox[first_msg].message = nullptr;
#endif // CONFIG_TELEGRAM_MESSAGE_SIZE
          _tgOutbox[first_msg].queued = false;
          _tgOutboxSize--;
          rlog_d(logTAG, "Message #%d removed from queue, outbox size: %d", first_msg, _tgOutboxSize);
        };
      };
    };
#else
    // Direct send
    if (xQueueReceive(_tgQueue, &inMsg, portMAX_DELAY) == pdPASS)
    {
      rlog_v(logTAG, "New message received: %s", inMsg->message);

      tgMessageItem_t tgMsg;
      memset(&tgMsg, 0, sizeof(tgMessageItem_t));
#if CONFIG_TELEGRAM_STATIC_MESSAGE_BUFFER
      memset(tgMsg.message, 0, CONFIG_TELEGRAM_MESSAGE_SIZE);
      strncpy(tgMsg.message, inMsg->message, CONFIG_TELEGRAM_MESSAGE_SIZE - 1);
      if (inMsg->message)
        free(inMsg->message);
#else
      tgMsg.message = inMsg->message;
#endif // CONFIG_TELEGRAM_MESSAGE_SIZE
      tgMsg.options = inMsg->options;
      tgMsg.timestamp = inMsg->timestamp;
      free(inMsg);
      inMsg = nullptr;

      // Trying to send a message to the Telegram API
      uint16_t trySend = 0;
      // Waiting for internet access
      while (statesInetWait(portMAX_DELAY))
      {
        trySend++;
        esp_err_t resSend = tgSendApi(&tgMsg);
        // If the send status has changed, send an event to the event loop
        if (resSend != resLast)
        {
          resLast = resSend;
          eventLoopPostError(RE_SYS_TELEGRAM_ERROR, resLast);
        };
        // If the message is sent (ESP_OK) or too long (ESP_ERR_NO_MEM) or an API error (ESP_ERR_INVALID_ARG - bad message?), then remove it from the heap
        if (resSend == ESP_OK)
        {
#if !CONFIG_TELEGRAM_STATIC_MESSAGE_BUFFER
          if (tgMsg.message)
            free(tgMsg.message);
          tgMsg.message = nullptr;
#endif // CONFIG_TELEGRAM_MESSAGE_SIZE
          break;
        }
        else
        {
          if (trySend <= CONFIG_TELEGRAM_MAX_ATTEMPTS)
          {
            if (resLast == ESP_ERR_INVALID_RESPONSE)
            {
              vTaskDelay(pdMS_TO_TICKS(CONFIG_TELEGRAM_FORBIDDEN_INTERVAL));
            }
            else
            {
              vTaskDelay(pdMS_TO_TICKS(CONFIG_TELEGRAM_SEND_INTERVAL));
            };
          }
          else
          {
            rlog_e(logTAG, "Failed to send message %s", tgMsg.message);
#if !CONFIG_TELEGRAM_STATIC_MESSAGE_BUFFER
            if (tgMsg.message)
              free(tgMsg.message);
            tgMsg.message = nullptr;
#endif // CONFIG_TELEGRAM_MESSAGE_SIZE
            break;
          };
        };
      };
    };
#endif // CONFIG_TELEGRAM_OUTBOX_ENABLE
  };

  // Delete task
  tgTaskDelete();
}

bool tgTaskCreate()
{
  if (!_tgTask)
  {
    if (!_tgQueue)
    {
#if CONFIG_TELEGRAM_STATIC_ALLOCATION
      _tgQueue = xQueueCreateStatic(CONFIG_TELEGRAM_QUEUE_SIZE, TELEGRAM_QUEUE_ITEM_SIZE, &(_tgQueueStorage[0]), &_tgQueueBuffer);
#else
      _tgQueue = xQueueCreate(CONFIG_TELEGRAM_QUEUE_SIZE, TELEGRAM_QUEUE_ITEM_SIZE);
#endif // CONFIG_TELEGRAM_STATIC_ALLOCATION
      if (!_tgQueue)
      {
        rloga_e("Failed to create a queue for sending notifications to Telegram!");
        eventLoopPostError(RE_SYS_TELEGRAM_ERROR, ESP_FAIL);
        return false;
      };
    };

#if CONFIG_TELEGRAM_STATIC_ALLOCATION
    _tgTask = xTaskCreateStaticPinnedToCore(tgTaskExec, tgTaskName, CONFIG_TELEGRAM_STACK_SIZE, nullptr, CONFIG_TASK_PRIORITY_TELEGRAM, _tgTaskStack, &_tgTaskBuffer, CONFIG_TASK_CORE_TELEGRAM);
#else
    xTaskCreatePinnedToCore(tgTaskExec, tgTaskName, CONFIG_TELEGRAM_STACK_SIZE, nullptr, CONFIG_TASK_PRIORITY_TELEGRAM, &_tgTask, CONFIG_TASK_CORE_TELEGRAM);
#endif // CONFIG_TELEGRAM_STATIC_ALLOCATION
    if (!_tgTask)
    {
      vQueueDelete(_tgQueue);
      rloga_e("Failed to create task for sending notifications to Telegram!");
      eventLoopPostError(RE_SYS_TELEGRAM_ERROR, ESP_FAIL);
      return false;
    }
    else
    {
      rloga_i("Task [ %s ] has been successfully started", tgTaskName);
      eventLoopPostError(RE_SYS_TELEGRAM_ERROR, ESP_OK);
      return true;
    };
  }
  else
  {
    return tgTaskResume();
  };
}

bool tgTaskSuspend()
{
  if ((_tgTask) && (eTaskGetState(_tgTask) != eSuspended))
  {
    vTaskSuspend(_tgTask);
    if (eTaskGetState(_tgTask) == eSuspended)
    {
      rloga_d("Task [ %s ] has been suspended", tgTaskName);
      eventLoopPostError(RE_SYS_TELEGRAM_ERROR, ESP_ERR_NOT_SUPPORTED);
      return true;
    }
    else
    {
      rloga_e("Failed to suspend task [ %s ]!", tgTaskName);
      eventLoopPostError(RE_SYS_TELEGRAM_ERROR, ESP_FAIL);
    };
  };
  return false;
}

bool tgTaskResume()
{
  if ((_tgTask) && (eTaskGetState(_tgTask) == eSuspended))
  {
    vTaskResume(_tgTask);
    if (eTaskGetState(_tgTask) != eSuspended)
    {
      rloga_i("Task [ %s ] has been successfully resumed", tgTaskName);
      eventLoopPostError(RE_SYS_TELEGRAM_ERROR, ESP_OK);
      return true;
    }
    else
    {
      rloga_e("Failed to resume task [ %s ]!", tgTaskName);
      eventLoopPostError(RE_SYS_TELEGRAM_ERROR, ESP_FAIL);
    };
  };
  return false;
}

bool tgTaskDelete()
{
  if (_tgQueue)
  {
    vQueueDelete(_tgQueue);
    rloga_v("The queue for sending notifications in Telegram has been deleted");
    _tgQueue = nullptr;
  };

  if (_tgTask)
  {
    vTaskDelete(_tgTask);
    _tgTask = nullptr;
    rloga_d("Task [ %s ] was deleted", tgTaskName);
  };

  return true;
}

// updates task
void tgTaskUpdatesExec(void *pvParameters)
{
  u_int32_t tick = 0;
  while (true)
  {
    rlog_d(logTAG, "tick %d", tick++);
    while (statesInetWait(portMAX_DELAY))
    {
      esp_err_t tgApiRequest = tgApi();

      if (tgApiRequest == ESP_OK)
      {
        rlog_d(logTAG, "Request to Telegram API done.");
        break;
      }
      else
      {
        rlog_e(logTAG, "Request to Telegram API failed. Error: %d", tgApiRequest);
        vTaskDelay(pdMS_TO_TICKS(1000));
        break;
      };
    };
    vTaskDelay(pdMS_TO_TICKS(10));
  };

  // Delete task
  tgTaskDelete();
}

bool tgTaskUpdatesCreate(QueueHandle_t *cmdQueue)
{
  _tgInboxQueue = cmdQueue;
  if (!_tgTaskUpdates)
  {
    if (!*_tgInboxQueue)
    {
      *_tgInboxQueue = xQueueCreate(4, sizeof(tgUpdateMessage_t *));
      if (!*_tgInboxQueue)
      {
        rloga_e("Failed to create a queue for incoming updates from Telegram!");
        eventLoopPostError(RE_SYS_TELEGRAM_ERROR, ESP_FAIL);
        return false;
      };
    };
    xTaskCreatePinnedToCore(tgTaskUpdatesExec, tgTaskUpdatesName, 4096, nullptr, CONFIG_TASK_PRIORITY_TELEGRAM, &_tgTaskUpdates, CONFIG_TASK_CORE_TELEGRAM);
    if (!_tgTaskUpdates)
    {
      vQueueDelete(*_tgInboxQueue);
      rloga_e("Failed to create task for Telegram updates!");
      eventLoopPostError(RE_SYS_TELEGRAM_ERROR, ESP_FAIL);
      return false;
    }
    else
    {
      rloga_d("Task [ %s ] has been successfully started", tgTaskUpdatesName);
      return true;
    };
  }
  else
  {
    return true;
  };
}
