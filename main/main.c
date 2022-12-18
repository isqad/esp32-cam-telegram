/* WiFi camera Telegram bot

   This code is in the MIT licensed
*/
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_http_client.h"

#include "wifi_sta.h"

#include "jsnm.h"

#define TASKS_STACK_SIZE                 10000
#define IDLE_PERIOD_MS                   10
#define MAX_JSON_TOKENS                  128
#define HTTP_CLIENT_TIMEOUT_MS           5000
#define HTTP_CLIENT_MAX_REDIRECTS        3
#define HTTP_CLIENT_USER_AGENT           "ESP32-Wifi-Camera Telegram Bot v1.0.0 (Home automation by Andy0x58)"
#define TG_API_BOT_ENDPOINT              "https://api.telegram.org/bot"
#define TG_API_BOT_GET_UPDATES_COMMAND   "/getUpdates"
#define UPDATE_ID_PARAM                  "?offset=%lu"
#define TG_GET_UPDATES_URL                TG_API_BOT_ENDPOINT CONFIG_ESP_TG_BOT_TOKEN TG_API_BOT_GET_UPDATES_COMMAND
#define TG_GET_UPDATES_WITH_UPDATE_ID_URL TG_GET_UPDATES_URL UPDATE_ID_PARAM

static uint32_t last_update_id = 0;

esp_event_loop_handle_t tg_bog_event_loop;

ESP_EVENT_DEFINE_BASE(TASK_EVENTS);

ESP_EVENT_DECLARE_BASE(TASK_EVENTS);
enum {
  EVENT_TG_BOT_GET_UPDATES
};

static esp_http_client_handle_t tg_bot_http_client_handle = NULL;

static int jsoneq(const char *json, jsmntok_t *tok, const char *s) {
  if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start &&
      strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
    return 0;
  }
  return -1;
}
 
static esp_err_t http_client_event_handler(esp_http_client_event_t* event)
{
  static char* http_response_buffer = NULL;

  static int64_t output_length = 0;
  static int64_t content_length = 0;

  switch (event->event_id)
  {
  case HTTP_EVENT_ERROR:
    ESP_LOGE(TAG, "http request errored");
    if (http_response_buffer != NULL)
    {
      free(http_response_buffer);
      http_response_buffer = NULL;
      output_length = 0;
      content_length = 0;
    }

    return ESP_FAIL;
    break;
  case HTTP_EVENT_ON_CONNECTED:
    ESP_LOGD(TAG, "Successfully connected!");
    break;
  case HTTP_EVENT_HEADERS_SENT:
    break;
  case HTTP_EVENT_ON_HEADER:
    break;
  case HTTP_EVENT_ON_DATA:
    if (esp_http_client_is_chunked_response(event->client)) 
    {
      ESP_LOGE(TAG, "Response chunked!");
      if (http_response_buffer != NULL)
      {
        free(http_response_buffer);
        http_response_buffer = NULL;
        output_length = 0;
        content_length = 0;
      }
      return ESP_FAIL;
    }

    if (http_response_buffer == NULL)
    {
      content_length = esp_http_client_get_content_length(event->client);
      ESP_LOGD(TAG, "Content-length: %lld", content_length);

      http_response_buffer = (char*)malloc(content_length + 1);
      if (http_response_buffer == NULL)
      {
        ESP_LOGE(TAG, "Not enough memory for allocate the http response");
        return ESP_FAIL;
      }
    }

    memcpy(http_response_buffer + output_length, event->data, event->data_len);
    output_length += event->data_len;

    if (output_length == content_length)
    {
      http_response_buffer[output_length] = '\0';
    }
    break;
  case HTTP_EVENT_ON_FINISH:
    if (http_response_buffer == NULL)
    {
      break;
    }

    ESP_LOGD(TAG, "Response data: %s", http_response_buffer);

    jsmn_parser p;
    jsmntok_t t[MAX_JSON_TOKENS]; /* We expect no more than MAX_JSON_TOKENS JSON tokens */
    jsmn_init(&p);

    int result = jsmn_parse(&p, http_response_buffer, strlen(http_response_buffer), t, MAX_JSON_TOKENS);
    
    if (result < 0)
    {
      switch (result)
      {
      case JSMN_ERROR_NOMEM:
        ESP_LOGE(TAG, "Not enough memory for parse JSON");
        break;
      case JSMN_ERROR_INVAL:
        ESP_LOGE(TAG, "Invalid character inside the JSON");
        break;
      case JSMN_ERROR_PART:
        ESP_LOGE(TAG, "Not full JSON");
        break;
      default:
        break;
      }

      free(http_response_buffer);
      http_response_buffer = NULL;
      output_length = 0;
      content_length = 0;

      return ESP_FAIL;
    }
    

    /* Assume the top-level element is an object */
    if (result < 1 || t[0].type != JSMN_OBJECT) 
    {
      ESP_LOGE(TAG, "Object extected at 0 token");
      free(http_response_buffer);
      http_response_buffer = NULL;
      output_length = 0;
      content_length = 0;

      return ESP_FAIL;
    }

    for (int i = 1; i < result; i++)
    {
      if (jsoneq(http_response_buffer, &t[i], "ok") == 0) 
      {
        i++;
      } 
      else if (jsoneq(http_response_buffer, &t[i], "result") == 0)
      {
        int j;
        if (t[i + 1].type != JSMN_ARRAY) {
          ESP_LOGE(TAG, "Exepected Array of objects");
          continue; /* We expect result to be an array of objects */
        }

        // Get last update
        int k = i + 2;
        for (j = 0; j < t[i + 1].size; j++) {
          jsmntok_t *update_token = &t[k];
          if (update_token->type != JSMN_OBJECT) 
          {
            ESP_LOGE(TAG, "Object expected!");
            break;
          }

          k++; // pass the Object token

          if (j < t[i + 1].size - 1)
          {
            while (k < MAX_JSON_TOKENS && t[k].end < update_token->end)
            {
              k++;
            }
            ESP_LOGD(TAG, "Found the next object %.*s", t[k].end - t[k].start, http_response_buffer + t[k].start);
            continue;
          }

          // Got last update
          if (jsoneq(http_response_buffer, &t[k], "update_id") == 0)
          {
            jsmntok_t *update_id_tok = &t[k + 1];

            char updated_id_buf[update_id_tok->end - update_id_tok->start];
            memcpy(updated_id_buf, http_response_buffer + update_id_tok->start, update_id_tok->end - update_id_tok->start);
            updated_id_buf[update_id_tok->end - update_id_tok->start] = '\0';

            last_update_id = strtoll(updated_id_buf, NULL, 10); // todo: pass with event

            break;
          }
          else
          {
            ESP_LOGE(TAG, "update_id expected!");
            break;
          }
        }

        break;
      }
      else
      {
        break;
        // ESP_LOGE(TAG, "Unexpected key! %.*s", t[i].end - t[i].start, http_response_buffer + t[i].start);
      }
    }

    free(http_response_buffer);
    http_response_buffer = NULL;
    output_length = 0;
    content_length = 0;
    
    if (esp_event_post_to(tg_bog_event_loop, TASK_EVENTS, EVENT_TG_BOT_GET_UPDATES, NULL, 0, portMAX_DELAY) == ESP_FAIL)
    {
      return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Finish http session!");
    break;
  case HTTP_EVENT_DISCONNECTED:
    ESP_LOGW(TAG, "Disconnected!");
    if (http_response_buffer != NULL)
    {
      free(http_response_buffer);
      http_response_buffer = NULL;
      output_length = 0;
      content_length = 0;
    }

    ESP_LOGD(TAG, "== Free heap size: %lu ==", esp_get_free_heap_size());
    break;
  default:
    break;
  }
  

  return ESP_OK;
}

void tg_get_updates(void* handler_arg, esp_event_base_t base, int32_t id, void* event_data)
{
  if (tg_bot_http_client_handle != NULL)
  {
    esp_http_client_close(tg_bot_http_client_handle);
    esp_http_client_cleanup(tg_bot_http_client_handle);
    tg_bot_http_client_handle = NULL;

    vTaskDelay(pdMS_TO_TICKS(2000));
  }

  int url_pattern_len = last_update_id > 0 ? strlen(TG_GET_UPDATES_WITH_UPDATE_ID_URL) + 16 : strlen(TG_GET_UPDATES_URL) + 1;
  char url[url_pattern_len];

  if (last_update_id > 0)
  {
    sprintf(url, TG_GET_UPDATES_WITH_UPDATE_ID_URL, last_update_id + 1);
  } 
  else
  {
    strcpy(url, TG_GET_UPDATES_URL);
  }

  ESP_LOGD(TAG, " Perform request to the URL: %s", url);
  
  const esp_http_client_config_t client_conf = {
    .url = url,
    .keep_alive_enable = true,
    .timeout_ms = HTTP_CLIENT_TIMEOUT_MS,
    .user_agent = HTTP_CLIENT_USER_AGENT,
    .max_redirection_count = HTTP_CLIENT_MAX_REDIRECTS,
    .event_handler = &http_client_event_handler
  };

  tg_bot_http_client_handle = esp_http_client_init(&client_conf);
  if (tg_bot_http_client_handle == NULL)
  {
    ESP_LOGE(TAG, "tg_bot_http_client_handle is NULL!");
    ESP_LOGI(TAG, "Restart!");
    return esp_restart();
  }

  ESP_LOGD(TAG, "Perform request to Get Updates!");

  if (esp_http_client_perform(tg_bot_http_client_handle) == ESP_FAIL) 
  {
    ESP_LOGE(TAG, "perform getUpdates failed!");
  }
}

void app_main(void)
{
  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // Connect to Wifi
  ESP_LOGD(TAG, "ESP_WIFI_MODE_STA");
  wifi_init_sta();

  esp_event_loop_args_t tg_bog_event_loop_args = {
      .queue_size = 1,
      .task_name = "Tg bot get updates", // task will be created
      .task_priority = tskIDLE_PRIORITY,
      .task_stack_size = TASKS_STACK_SIZE,
      .task_core_id = tskNO_AFFINITY
  };

  ESP_ERROR_CHECK(esp_event_loop_create(&tg_bog_event_loop_args, &tg_bog_event_loop));

  ESP_ERROR_CHECK(esp_event_handler_register_with(tg_bog_event_loop, TASK_EVENTS, EVENT_TG_BOT_GET_UPDATES, tg_get_updates, NULL));

  ESP_ERROR_CHECK(esp_event_post_to(tg_bog_event_loop, TASK_EVENTS, EVENT_TG_BOT_GET_UPDATES, NULL, 0, portMAX_DELAY));

  for( ;; )
  {
      vTaskDelay(pdMS_TO_TICKS(IDLE_PERIOD_MS));
  }
}
