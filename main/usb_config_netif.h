#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up a small local IP interface on the SAME USB NCM/ECM/RNDIS
 * link already used for Wi-Fi<->USB bridging (cmd_wifi.c) - active
 * whenever Wi-Fi STA is NOT connected. Default: 192.168.7.1/24, DHCP
 * server enabled - so the host PC gets a real IP directly over the USB
 * cable (instead of the 169.254.x.x APIPA address it self-assigns when
 * nothing answers DHCP), and can reach the wifi_api HTTP config server
 * (see wifi_api.h) at http://192.168.7.1/ with no separate Wi-Fi hop.
 *
 * Must be called AFTER initialise_wifi() (cmd_wifi.c), because it needs
 * esp_netif_init() to already have run.
 *
 * IMPORTANT: for this to work, tinyusb_net_init() in usb_dongle_main.c
 * must be wired to usb_net_recv_dispatch()/usb_net_free_tx_buffer()
 * below instead of the original wifi_recv_callback()/wifi_buffer_free()
 * - see usb_dongle_main.c.
 */
esp_err_t usb_config_netif_start(void);

/**
 * @brief on_recv_callback for tinyusb_net_init(): routes each frame
 * received from the USB host either into the existing Wi-Fi bridge
 * (cmd_wifi.c's wifi_recv_callback(), when STA is connected) or into
 * the local config netif started by usb_config_netif_start() above
 * (otherwise).
 */
esp_err_t usb_net_recv_dispatch(void *buffer, uint16_t len, void *ctx);

/**
 * @brief free_tx_buffer for tinyusb_net_init(): frees a just-transmitted
 * buffer correctly, whichever of the two paths above produced it.
 */
void usb_net_free_tx_buffer(void *buffer, void *ctx);

#ifdef __cplusplus
}
#endif
