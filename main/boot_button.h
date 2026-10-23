#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start a background task that watches the BOOT button (GPIO0 by
 * default - see BOOT_BUTTON_GPIO in boot_button.c) and, when held down
 * continuously for BOOT_BUTTON_HOLD_MS (5000ms by default), disconnects
 * the current Wi-Fi connection and re-enables the setup channels
 * (wifi_api's "ESP32-WiFi-Setup" AP + the USB config netif) so a new
 * network can be picked without re-flashing or power-cycling.
 *
 * Safe to call once; the task runs for the lifetime of the app.
 */
esp_err_t boot_button_monitor_start(void);

#ifdef __cplusplus
}
#endif
