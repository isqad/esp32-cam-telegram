/* WiFi camera Telegram bot

   This code is in the MIT licensed
*/
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_http_client.h"

#include "wifi_sta.h"

#include "jsnm.h"

#define TASKS_STACK_SIZE 8192
#define IDLE_PERIOD_MS 10

#define HTTP_CLIENT_TIMEOUT_MS 5000
#define HTTP_CLIENT_MAX_REDIRECTS 3
#define HTTP_CLIENT_USER_AGENT "ESP32-Wifi-Camera Telegram Bot v1.0.0 (Home automation by Andy0x58)"

#define TG_API_BOT_ENDPOINT             "https://api.telegram.org/bot"

#define TG_API_BOT_GET_UPDATES_COMMAND  "/getUpdates"
#define TG_GET_UDATES_URL               TG_API_BOT_ENDPOINT CONFIG_ESP_TG_BOT_TOKEN TG_API_BOT_GET_UPDATES_COMMAND

static char* http_response_buffer = NULL;

static uint32_t update_id = 0;

esp_event_loop_handle_t tg_bog_event_loop;

ESP_EVENT_DEFINE_BASE(TASK_EVENTS);

ESP_EVENT_DECLARE_BASE(TASK_EVENTS);
enum {
  EVENT_TG_BOT_GET_UPDATES
};

static esp_http_client_handle_t tg_bot_http_client_handle;
 
static esp_err_t http_client_event_handler(esp_http_client_event_t* event)
{
  static int64_t output_length;
  static int64_t content_length;

  switch (event->event_id)
  {
  case HTTP_EVENT_ERROR:
    ESP_LOGI(TAG, "http request errored");
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
    ESP_LOGI(TAG, "Successfully connected to %s", TG_GET_UDATES_URL);
    break;
  case HTTP_EVENT_HEADERS_SENT:
    break;
  case HTTP_EVENT_ON_HEADER:
    break;
  case HTTP_EVENT_ON_DATA:
    if (esp_http_client_is_chunked_response(event->client)) 
    {
      ESP_LOGE(TAG, "Response chunked!");
      return ESP_FAIL;
    }

    if (http_response_buffer == NULL)
    {
      output_length = 0;
      content_length = esp_http_client_get_content_length(event->client);
      ESP_LOGI(TAG, "Content-length: %lld", content_length);

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
    if (http_response_buffer != NULL)
    {
      ESP_LOGI(TAG, "Response data: %s", http_response_buffer);

      free(http_response_buffer);
      http_response_buffer = NULL;
      output_length = 0;
      content_length = 0;

      if (esp_event_post_to(tg_bog_event_loop, TASK_EVENTS, EVENT_TG_BOT_GET_UPDATES, NULL, 0, portMAX_DELAY) == ESP_FAIL)
      {
        return ESP_FAIL;
      }
    }

    ESP_LOGI(TAG, "Finish http session with %s", TG_GET_UDATES_URL);
    break;
  case HTTP_EVENT_DISCONNECTED:
    ESP_LOGW(TAG, "Disconnected from %s", TG_GET_UDATES_URL);
    if (http_response_buffer != NULL)
    {
      free(http_response_buffer);
      http_response_buffer = NULL;
      output_length = 0;
      content_length = 0;
    }
    break;
  default:
    break;
  }
  

  return ESP_OK;
}

static const esp_http_client_config_t client_conf = {
  .url = TG_GET_UDATES_URL,
  .keep_alive_enable = true,
  .timeout_ms = HTTP_CLIENT_TIMEOUT_MS,
  .user_agent = HTTP_CLIENT_USER_AGENT,
  .max_redirection_count = HTTP_CLIENT_MAX_REDIRECTS,
  .event_handler = &http_client_event_handler
};

void tg_get_updates(void* handler_arg, esp_event_base_t base, int32_t id, void* event_data)
{
  if (tg_bot_http_client_handle == NULL)
  {
    ESP_LOGE(TAG, "http client is not initialized yet!");
    return;
  }
  ESP_LOGI(TAG, "Perform request to Get Updates!");

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
  ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
  wifi_init_sta();

  tg_bot_http_client_handle = esp_http_client_init(&client_conf);
  if (tg_bot_http_client_handle == NULL)
  {
    ESP_LOGE(TAG, "tg_bot_http_client_handle is NULL!");
    ESP_LOGI(TAG, "Restart!");
    return esp_restart();
  }

  esp_event_loop_args_t tg_bog_event_loop_args = {
      .queue_size = 1,
      .task_name = "Tg bot get updates", // task will be created
      .task_priority = (UBaseType_t)5U,
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
