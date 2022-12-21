#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "esp_tls.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "esp_http_client.h"
#include "aht.h"
#include "bmp280.h"
#include "esp_err.h"

#ifdef CONFIG_EXAMPLE_I2C_ADDRESS_GND
#define ADDR AHT_I2C_ADDRESS_GND
#endif
#ifdef CONFIG_EXAMPLE_I2C_ADDRESS_VCC
#define ADDR AHT_I2C_ADDRESS_VCC
#endif

#ifdef CONFIG_EXAMPLE_TYPE_AHT20
#define AHT_TYPE AHT_TYPE_AHT20
#endif

#ifndef APP_CPU_NUM
#define APP_CPU_NUM PRO_CPU_NUM
#endif

#define uS_TO_S_FACTOR 1000000  /* Conversion factor for micro seconds to seconds */
#define TIME_TO_SLEEP  1800        /* Time ESP32 will go to sleep (in seconds) */
#define MAX_HTTP_RECV_BUFFER 512
#define MAX_HTTP_OUTPUT_BUFFER 2048

RTC_DATA_ATTR int bootCount = 0;

float temperature = 0.0;
float humidity = 0.0;
float pressure = 1050.0;

static const char *TAG = "HTTP_CLIENT";

/**
 * @brief Method to print the reason by which ESP32 has been awaken from sleep
 *
 */
void print_wakeup_reason(){
  esp_sleep_wakeup_cause_t wakeup_reason;

  wakeup_reason = esp_sleep_get_wakeup_cause();

  switch(wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0 : printf("Wakeup caused by external signal using RTC_IO \n"); break;
    case ESP_SLEEP_WAKEUP_EXT1 : printf("Wakeup caused by external signal using RTC_CNTL \n"); break;
    case ESP_SLEEP_WAKEUP_TIMER : printf("Wakeup caused by timer \n"); break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD : printf("Wakeup caused by touchpad \n"); break;
    case ESP_SLEEP_WAKEUP_ULP : printf("Wakeup caused by ULP program \n"); break;
    default : printf("Wakeup was not caused by deep sleep: %d\n",wakeup_reason); break;
  }
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer;  // Buffer to store response of http request from event handler
    static int output_len;       // Stores number of bytes read
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            /*
             *  Check for chunked encoding is added as the URL for chunked encoding used in this example returns binary data.
             *  However, event handler can also be used in case chunked encoding is used.
             */
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // If user_data buffer is configured, copy the response into the buffer
                if (evt->user_data) {
                    memcpy(evt->user_data + output_len, evt->data, evt->data_len);
                } else {
                    if (output_buffer == NULL) {
                        output_buffer = (char *) malloc(esp_http_client_get_content_length(evt->client));
                        output_len = 0;
                        if (output_buffer == NULL) {
                            ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                            return ESP_FAIL;
                        }
                    }
                    memcpy(output_buffer + output_len, evt->data, evt->data_len);
                }
                output_len += evt->data_len;
            }

            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            if (output_buffer != NULL) {
                // Response is accumulated in output_buffer. Uncomment the below line to print the accumulated response
                // ESP_LOG_BUFFER_HEX(TAG, output_buffer, output_len);
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            int mbedtls_err = 0;
            esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data, &mbedtls_err, NULL);
            if (err != 0) {
                ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
                ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
            if (output_buffer != NULL) {
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
            esp_http_client_set_header(evt->client, "From", "user@example.com");
            esp_http_client_set_header(evt->client, "Accept", "text/html");
            esp_http_client_set_redirection(evt->client);
            break;
    }
    return ESP_OK;
}

static void http_rest_with_url(void)
{
    ESP_LOGI(TAG, "Inside http_rest_with_url");
    char local_response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};
    char query_buffer[256];
    snprintf(query_buffer, 256, "device=terraza&temp=%.2f&humidity=%.2f&pressure=%.2f", temperature, humidity, pressure);
    ESP_LOGI(TAG, "Query Buffer set, %s", query_buffer);
    /**
     * NOTE: All the configuration parameters for http_client must be spefied either in URL or as host and path parameters.
     * If host and path parameters are not set, query parameter will be ignored. In such cases,
     * query parameter should be specified in URL.
     *
     * If URL as well as host and path parameters are specified, values of host and path will be considered.
     */
    // .query = "device=terraza&temp=25&humidity=50&pressure=1000",
    // .query = query_buffer,
    esp_http_client_config_t config = {
        .host = "homestats.test",
        .path = "/api/home/test",
        // .query = "device=terraza&temp=25&humidity=50&pressure=1000",
        .event_handler = _http_event_handler,
        .user_data = local_response_buffer,        // Pass address of local buffer to get response
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    // // Set Headers
    // esp_http_client_set_header(client, "Content-Type", "application/json");
    // esp_http_client_set_header(client, "Authorization", "Bearer ");

    // ESP_LOGI(TAG, "Client initiated, making a GET to homestats.test");
    // // GET
    // esp_err_t err = esp_http_client_perform(client);
    // if (err == ESP_OK) {
    //     ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %lld",
    //             esp_http_client_get_status_code(client),
    //             esp_http_client_get_content_length(client));
    // } else {
    //     ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    // }
    // ESP_LOG_BUFFER_HEX(TAG, local_response_buffer, strlen(local_response_buffer));

    // POST
    // char *post_data = "{\"device\":\"Terraza\",\"temperature\":\"%.2f\",\"humidity\":\"%.2f\",\"pressure\":\"%.2f\"}";
    // snprintf(post_data, strlen(post_data), "{\"device\":\"Terraza\",\"temperature\":\"%.2f\",\"humidity\":\"%.2f\",\"pressure\":\"%.2f\"}", temperature, humidity, pressure);
    // const char *post_data = "{\"data\":[\"device\":\"Terraza\",\"temp\":\"25\",\"humidity\":\"50\",\"pressure\":\"1000\"]}";
    esp_http_client_set_url(client, "http://homestats.test/api/home/test");
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    // esp_http_client_set_header(client, "Authorization: Bearer", "5FM8IA1I7sYu9LcWUdsFbGlYsCB4x316pC7DTOyt");
    esp_http_client_set_header(client, "Authorization", "Bearer 5FM8IA1I7sYu9LcWUdsFbGlYsCB4x316pC7DTOyt");
    // esp_http_client_set_post_field(client, post_data, strlen(post_data));
    esp_http_client_set_post_field(client, query_buffer, strlen(query_buffer));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %lld",
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}


static void http_client_task(void *pvParameters)
{
    http_rest_with_url();

    ESP_LOGI(TAG, "Finish http example");
    vTaskDelete(NULL);
}


void measure_task(void *pvParameters)
{
    aht_t dev = { 0 };
    dev.mode = AHT_MODE_NORMAL;
    dev.type = AHT_TYPE;

    ESP_ERROR_CHECK(aht_init_desc(&dev, ADDR, 0, CONFIG_EXAMPLE_I2C_MASTER_SDA, CONFIG_EXAMPLE_I2C_MASTER_SCL));
    ESP_ERROR_CHECK(aht_init(&dev));

    bool calibrated;
    ESP_ERROR_CHECK(aht_get_status(&dev, NULL, &calibrated));
    if (calibrated)
        ESP_LOGI(TAG, "Sensor calibrated");
    else
        ESP_LOGW(TAG, "Sensor not calibrated!");

    // float temperature, humidity;

    // while (1)
    // {
        esp_err_t res = aht_get_data(&dev, &temperature, &humidity);
        if (res != ESP_OK)
            ESP_LOGE(TAG, "Error reading data: %d (%s)", res, esp_err_to_name(res));
            // ESP_LOGI(TAG, "Temperature: %.1f°C, Humidity: %.2f%%", temperature, humidity);
        // else
            // ESP_LOGE(TAG, "Error reading data: %d (%s)", res, esp_err_to_name(res));

// TODO: Find how to exit the task, without waiting that long.
        vTaskDelay(pdMS_TO_TICKS(10000));
        // exit(1);
    // }
}

/**
 * @brief Main App code. HEre we invoke all other functions.
 *
 */
void app_main(void)
{
    // Wait for everything to init.
    vTaskDelay(500 / portTICK_PERIOD_MS);

    print_wakeup_reason();

    ++bootCount;

    // WIFI connection using code from examples and sending GET request as HTTP Client.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Make a reading on AHT20 and BMP280
    ESP_ERROR_CHECK(i2cdev_init());
    // xTaskCreatePinnedToCore(measure_task, TAG, configMINIMAL_STACK_SIZE * 8, NULL, 5, NULL, APP_CPU_NUM);
    xTaskCreate(&measure_task, "measure_task", 8192, NULL, 5, NULL);
    ESP_LOGI(TAG, "Temperature: %.1f°C, Humidity: %.2f%%", temperature, humidity);

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());
    ESP_LOGI(TAG, "Connected to AP, begin http example");
    xTaskCreate(&http_client_task, "http_client_task", 8192, NULL, 5, NULL);

    // Set the deep sleep time.
    uint64_t sleepTime = TIME_TO_SLEEP * uS_TO_S_FACTOR;
    esp_sleep_enable_timer_wakeup(sleepTime);
    vTaskDelay(500 / portTICK_PERIOD_MS);

    printf("Sleeping now. %d \n", bootCount);
    fflush(stdout);
    esp_deep_sleep_start();

}
