#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sleep.h"

#define uS_TO_S_FACTOR 1000000  /* Conversion factor for micro seconds to seconds */
#define TIME_TO_SLEEP  300        /* Time ESP32 will go to sleep (in seconds) */

RTC_DATA_ATTR int bootCount = 0;

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

    // Set the deep sleep time.
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
    vTaskDelay(500 / portTICK_PERIOD_MS);

    printf("Sleeping now. %d \n", bootCount);
    fflush(stdout);
    esp_deep_sleep_start();

}
