/*
 * boot_button.c
 *
 * Hold the BOOT button for 5 seconds to disconnect the current Wi-Fi
 * network and bring the setup channels (wifi_api.c's "ESP32-WiFi-Setup"
 * AP @ 192.168.4.1, and the USB config netif @ 192.168.7.1) back up, so
 * a different network can be picked from the same scan/connect UI used
 * for initial setup - no re-flash or power cycle needed.
 *
 * GPIO0 is the BOOT button on essentially every ESP32/ESP32-S3 dev
 * board (it's also the boot-mode strapping pin, but reading it as a
 * plain input after boot is the standard, safe way apps use it as a
 * user button). Change BOOT_BUTTON_GPIO below if your board wires it
 * differently.
 */

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "wifi_api.h"
#include "boot_button.h"

#define BOOT_BUTTON_GPIO     GPIO_NUM_0
#define BOOT_BUTTON_HOLD_MS  5000
#define BOOT_BUTTON_POLL_MS  50

static const char *TAG = "boot_btn";

static void boot_button_task(void *arg)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* BOOT is active-low */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    uint32_t held_ms   = 0;
    bool     triggered = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BOOT_BUTTON_POLL_MS));

        bool pressed = (gpio_get_level(BOOT_BUTTON_GPIO) == 0);

        if (pressed) {
            held_ms += BOOT_BUTTON_POLL_MS;
            if (!triggered && held_ms >= BOOT_BUTTON_HOLD_MS) {
                triggered = true;
                ESP_LOGW(TAG, "BOOT held %ums - resetting Wi-Fi for reconfiguration", (unsigned)held_ms);
                wifi_api_reset_for_reconfigure();
            }
        } else {
            held_ms   = 0;
            triggered = false; /* re-arm once the button is released */
        }
    }
}

esp_err_t boot_button_monitor_start(void)
{
    BaseType_t ok = xTaskCreate(boot_button_task, "boot_btn", 3072, NULL, 3, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_FAIL;
}
