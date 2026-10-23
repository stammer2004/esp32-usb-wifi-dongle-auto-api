#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the Wi-Fi setup Access Point + HTTP configuration API.
 *
 * This project bridges Wi-Fi <-> USB at layer 2 (see cmd_wifi.c,
 * esp_wifi_internal_reg_rxcb/esp_wifi_internal_tx), so the ESP32 itself
 * has no reachable IP address on the STA/USB bridge path. To make a
 * config UI reachable, this switches Wi-Fi to WIFI_MODE_APSTA and hosts
 * the API on the already-present (but normally dormant) soft-AP netif
 * (default 192.168.4.1, SSID "ESP32-WiFi-Setup", open).
 *
 * Exposes, on the setup AP:
 *   GET  /                  -> minimal web UI (scan + connect form)
 *   GET  /api/wifi/scan     -> JSON list of nearby access points
 *   GET  /api/wifi/status   -> JSON with current STA connection status
 *   POST /api/wifi/connect  -> body {"ssid":"...","password":"..."}
 *                              acks immediately, then calls the existing
 *                              wifi_cmd_sta_join() from cmd_wifi.c, which
 *                              switches Wi-Fi back to STA-only mode - the
 *                              setup AP (and this HTTP connection) then
 *                              disappears, exactly like after SmartConfig
 *                              completes. Credentials are persisted to
 *                              FLASH the same way ESP-Touch credentials
 *                              already are (WIFI_STORAGE_FLASH).
 *
 * Must be called AFTER initialise_wifi() (cmd_wifi.c) has run, i.e.
 * after esp_wifi_init()/esp_wifi_start(). Safe to call once; call
 * wifi_api_stop() before calling again.
 *
 * Only compiled in when CFG_TUD_NCM || CFG_TUD_ECM_RNDIS (same guard as
 * cmd_wifi.c) - otherwise both functions are no-ops returning
 * ESP_ERR_NOT_SUPPORTED.
 */
esp_err_t wifi_api_start(void);

/**
 * @brief Disconnect the current Wi-Fi connection (cleanly - reuses
 * cmd_wifi.c's wif_cmd_disconnect_wifi(), which correctly disables
 * auto-reconnect before disconnecting) and re-enable the setup
 * channels: the "ESP32-WiFi-Setup" AP and, since the USB config netif
 * only needs Wi-Fi to be disconnected (not any particular mode), it
 * comes back on its own once disconnected - see usb_config_netif.c.
 *
 * Intended to be wired to a physical trigger (see boot_button.h) so a
 * different network can be selected without re-flashing or power
 * cycling. Safe to call at any time, connected or not.
 */
esp_err_t wifi_api_reset_for_reconfigure(void);

/**
 * @brief Stop the HTTP server and its URI handlers.
 * Does not revert Wi-Fi mode back from APSTA on its own.
 */
esp_err_t wifi_api_stop(void);

#ifdef __cplusplus
}
#endif
