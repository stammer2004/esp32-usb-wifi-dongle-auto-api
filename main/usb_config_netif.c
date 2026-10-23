/*
 * usb_config_netif.c
 *
 * Gives the ESP32 its own reachable IP address directly on the USB
 * NCM/ECM/RNDIS link (192.168.7.1/24 with a DHCP server), active only
 * while Wi-Fi STA is not connected, so the wifi_api HTTP config server
 * can be reached straight over USB during initial setup - no separate
 * Wi-Fi hop needed, and no more 169.254.x.x APIPA guesswork on Windows.
 *
 * HOW IT WORKS
 * ------------
 * cmd_wifi.c's tinyusb_net_init() only knows how to hand raw Ethernet
 * frames to a single pair of callbacks (on_recv_callback/free_tx_buffer)
 * - it has no IP stack of its own. Normally those callbacks bridge
 * frames straight to/from the Wi-Fi STA interface at layer 2
 * (wifi_recv_callback / pkt_wifi2usb), bypassing lwIP entirely.
 *
 * This file adds a second destination for those frames: a real
 * esp_netif (with its own lwIP instance, DHCP server, etc.), wired to
 * tinyusb_net via a small custom I/O driver glue (the same pattern
 * ESP-IDF's own Ethernet/PPP netif glues use: esp_netif_driver_base_t +
 * esp_netif_driver_ifconfig_t, see esp_netif_types.h).
 *
 * usb_net_recv_dispatch() (registered as tinyusb_net's on_recv_callback
 * in usb_dongle_main.c) picks, per received frame, whether to bridge it
 * to Wi-Fi (STA connected) or feed it into this local netif (STA not
 * connected). usb_net_free_tx_buffer() mirrors that choice for the
 * transmit side, since pkt_wifi2usb()'s buffers (cmd_wifi.c, Wi-Fi
 * driver-owned) and this file's own heap-copied buffers must be freed
 * with different mechanisms - see the comment there for the exact
 * invariant this relies on.
 */

#include "tinyusb.h"

#if CFG_TUD_NCM || CFG_TUD_ECM_RNDIS

#include <string.h>
#include <stdlib.h>

#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_log.h"
#include "esp_private/wifi.h"   /* esp_wifi_internal_free_rx_buffer() */

#include "tinyusb_net.h"
#include "usb_config_netif.h"

static const char *TAG = "usb_netif";

/* --- reused from cmd_wifi.c (declared there without `static`) --------- */
extern bool s_wifi_is_connected;
extern esp_err_t wifi_recv_callback(void *buffer, uint16_t len, void *ctx);

static esp_netif_t *s_usb_netif = NULL;

/* ---------------------------------------------------------------------- */
/* esp_netif <-> tinyusb_net I/O driver glue                              */
/* ---------------------------------------------------------------------- */

typedef struct {
    esp_netif_driver_base_t base;
} usb_netif_driver_t;

static usb_netif_driver_t s_driver = { 0 };

/* esp_netif -> USB host. Called by lwIP whenever our local netif (DHCP
 * server replies, ARP replies, TCP segments for the HTTP server, ...)
 * wants to send a frame out. tinyusb_net_send() consumes/copies `buffer`
 * synchronously (see tud_network_xmit_cb() in tinyusb_net.c) before
 * returning, so our own heap copy is safe to free as soon as it's done -
 * see usb_net_free_tx_buffer() below, which does exactly that. */
static esp_err_t usb_netif_transmit(void *h, void *buffer, size_t len)
{
    void *copy = malloc(len);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, buffer, len);
    if (tinyusb_net_send(copy, (uint16_t)len, copy) != ESP_OK) {
        free(copy);
    }
    return ESP_OK;
}

static void usb_netif_free_rx_buffer(void *h, void *buffer)
{
    /* We always pass eb=NULL to esp_netif_receive() below (see
     * usb_net_recv_dispatch), so lwIP never calls back here with
     * anything to free - nothing to do. */
    (void)h;
    (void)buffer;
}

static esp_err_t usb_netif_post_attach(esp_netif_t *esp_netif, void *args)
{
    usb_netif_driver_t *driver = (usb_netif_driver_t *)args;
    driver->base.netif = esp_netif;

    esp_netif_driver_ifconfig_t driver_ifconfig = {
        .handle              = driver,
        .transmit            = usb_netif_transmit,
        .driver_free_rx_buffer = usb_netif_free_rx_buffer,
    };

    return esp_netif_set_driver_config(esp_netif, &driver_ifconfig);
}

/* ---------------------------------------------------------------------- */
/* Public API                                                              */
/* ---------------------------------------------------------------------- */

esp_err_t usb_config_netif_start(void)
{
    if (s_usb_netif) {
        ESP_LOGW(TAG, "usb_config_netif already started");
        return ESP_ERR_INVALID_STATE;
    }

    static esp_netif_ip_info_t usb_ip_info = {
        .ip      = { .addr = ESP_IP4TOADDR(192, 168, 7, 1) },
        .gw      = { .addr = ESP_IP4TOADDR(192, 168, 7, 1) },
        .netmask = { .addr = ESP_IP4TOADDR(255, 255, 255, 0) },
    };

    static const esp_netif_inherent_config_t usb_inherent_cfg = {
        .flags      = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP,
        .ip_info    = &usb_ip_info,
        .if_key     = "USB_CFG_DEF",
        .if_desc    = "usbcfg",
        .route_prio = 5,
    };

    /* IMPORTANT: use the generic Ethernet-style lwIP glue here, NOT
     * ESP_NETIF_NETSTACK_DEFAULT_WIFI_AP - that one's low_level_output
     * (wlanif.c) calls straight into esp_wifi internals and ignores our
     * driver_ifconfig.transmit callback entirely, which crashes (jumps
     * through a null function pointer). ESP_NETIF_NETSTACK_DEFAULT_ETH
     * is the media-agnostic glue meant for exactly this - custom L2
     * drivers (same one esp_eth/custom MAC drivers use) - and correctly
     * calls our usb_netif_transmit() above. DHCP server / static IP
     * behavior comes from the inherent-config flags below, independent
     * of which netstack glue is selected. */
    esp_netif_config_t cfg = {
        .base   = &usb_inherent_cfg,
        .driver = NULL, /* set via esp_netif_attach() below */
        .stack  = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };

    s_usb_netif = esp_netif_new(&cfg);
    if (!s_usb_netif) {
        ESP_LOGE(TAG, "esp_netif_new() failed");
        return ESP_FAIL;
    }

    s_driver.base.post_attach = usb_netif_post_attach;
    esp_err_t err = esp_netif_attach(s_usb_netif, &s_driver);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_attach() failed: %s", esp_err_to_name(err));
        return err;
    }

    /* No real driver-side "start" event source (unlike Wi-Fi/Ethernet),
     * so bring the interface up directly - this is what actually starts
     * the DHCP server for an AP-style netstack. Returns void (it's an
     * esp_event-style handler function, not a regular esp_err_t API). */
    esp_netif_action_start(s_usb_netif, NULL, 0, NULL);

    ESP_LOGI(TAG, "USB config netif up - http://192.168.7.1/ (until Wi-Fi STA connects)");
    return ESP_OK;
}

/* ---------------------------------------------------------------------- */
/* tinyusb_net callbacks (registered in usb_dongle_main.c)                */
/* ---------------------------------------------------------------------- */

esp_err_t usb_net_recv_dispatch(void *buffer, uint16_t len, void *ctx)
{
    if (s_wifi_is_connected) {
        return wifi_recv_callback(buffer, len, ctx);
    }
    if (s_usb_netif) {
        return esp_netif_receive(s_usb_netif, buffer, len, NULL);
    }
    /* Very early boot, before usb_config_netif_start() has run yet -
     * nothing we can do with this frame. */
    return ESP_OK;
}

void usb_net_free_tx_buffer(void *buffer, void *ctx)
{
    /* Mirrors the branch in usb_net_recv_dispatch(): while STA is
     * connected, the only thing calling tinyusb_net_send() is
     * pkt_wifi2usb() in cmd_wifi.c, with a Wi-Fi-driver-owned buffer.
     * Otherwise, it's our own usb_netif_transmit() heap copy above. */
    if (s_wifi_is_connected) {
        esp_wifi_internal_free_rx_buffer(buffer);
    } else {
        free(buffer);
    }
}

#else /* !(CFG_TUD_NCM || CFG_TUD_ECM_RNDIS) */

#include "esp_err.h"
#include "usb_config_netif.h"

esp_err_t usb_config_netif_start(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t usb_net_recv_dispatch(void *buffer, uint16_t len, void *ctx) { return ESP_OK; }
void usb_net_free_tx_buffer(void *buffer, void *ctx) { }

#endif
