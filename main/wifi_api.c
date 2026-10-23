/*
 * wifi_api.c
 *
 * Adds a temporary "setup" Wi-Fi Access Point + HTTP REST API to
 * esp32-usb-wifi-dongle-auto, so the target STA network (SSID +
 * password) can be scanned and selected from a phone/laptop browser -
 * as an additional, alternative way to provision credentials, next to
 * the existing ESP-Touch SmartConfig flow already in the project.
 *
 * WHY A SOFT-AP AND NOT THE USB NETWORK LINK OR THE JOINED WI-FI
 * ----------------------------------------------------------------
 * This project bridges Wi-Fi <-> USB at layer 2: cmd_wifi.c registers
 * a raw Wi-Fi RX callback (esp_wifi_internal_reg_rxcb) that forwards
 * frames straight to the USB network interface (pkt_wifi2usb) and back
 * (wifi_recv_callback / esp_wifi_internal_tx), bypassing the ESP32's
 * own IP/lwIP stack on that path entirely. That means the ESP32 itself
 * never gets an IP address of its own on the STA<->USB bridge - only
 * the connected host PC does (via DHCP from the real router). So an
 * HTTP server can't be reached over the USB link or over the joined
 * Wi-Fi network.
 *
 * The already-present (but normally dormant) `ap_netif` soft-AP
 * interface, however, DOES run a normal IP stack + DHCP server
 * (default 192.168.4.1/24), completely independent of the STA bridge.
 * This file switches Wi-Fi into APSTA mode and hosts the config
 * API/UI there - connect your phone/laptop to the "ESP32-WiFi-Setup"
 * network, open http://192.168.4.1/, and configure the *other*
 * (STA/bridge) network from there.
 *
 * Once you submit valid credentials, the existing wifi_cmd_sta_join()
 * (cmd_wifi.c) is called, which switches Wi-Fi back to STA-only mode -
 * the setup AP (and this very HTTP connection) then disappears,
 * exactly like it does once SmartConfig completes. This is expected:
 * the dongle goes back to being a pure bridge for normal operation.
 * Credentials are persisted to FLASH via the WIFI_STORAGE_FLASH the
 * project already configures in initialise_wifi().
 */

#include "tinyusb.h"

#if CFG_TUD_NCM || CFG_TUD_ECM_RNDIS

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "cJSON.h"
#include "class/net/net_device.h"   /* tud_network_link_state() */

#include "wifi_api.h"

static const char *TAG = "wifi_api";

/* --- reused from cmd_wifi.c (declared there without `static`) --------- */
extern esp_err_t wifi_cmd_sta_join(const char *ssid, const char *pass);
extern esp_err_t wifi_cmd_stop_smart_config(void);

/* Scan-result cache populated by cmd_wifi.c's own scan_done_handler().
 * We deliberately do NOT call esp_wifi_scan_get_ap_records() ourselves:
 * that call frees the WiFi driver's internal scan list, and cmd_wifi.c's
 * pre-existing scan_done_handler (registered long before wifi_api_start()
 * runs) always consumes it first for the console "scan" command - a
 * second call from here would then always see zero results. Reading this
 * cache instead avoids that fight entirely. See cmd_wifi.c. */
#define WIFI_SCAN_CACHE_MAX 30
extern wifi_ap_record_t g_wifi_scan_cache[WIFI_SCAN_CACHE_MAX];
extern uint16_t g_wifi_scan_cache_count;

/* --- setup-AP configuration -------------------------------------------- */
#define WIFI_API_AP_SSID                "ESP32-WiFi-Setup"
#define WIFI_API_AP_PASSWORD            ""      /* empty = open network  */
#define WIFI_API_AP_CHANNEL             1
#define WIFI_API_REQ_BODY_MAX           512
#define WIFI_API_POST_CONNECT_DELAY_MS  300     /* let the ack leave the radio before mode switch */
#define WIFI_API_SCAN_TIMEOUT_MS        10000

static httpd_handle_t s_server = NULL;

/* Signalled by scan_done_relay() (registered once in wifi_api_start(),
 * after cmd_wifi.c's own scan_done_handler - see the comment above and
 * in wifi_api_start() for why registration order here matters). */
static EventGroupHandle_t s_scan_ev_grp = NULL;
#define SCAN_DONE_BIT BIT0

static void scan_done_relay(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (s_scan_ev_grp) {
        xEventGroupSetBits(s_scan_ev_grp, SCAN_DONE_BIT);
    }
}

/* ---------------------------------------------------------------------- */
/* Helpers                                                                */
/* ---------------------------------------------------------------------- */

static const char *authmode_to_str(wifi_auth_mode_t mode)
{
    switch (mode) {
        case WIFI_AUTH_OPEN:            return "open";
        case WIFI_AUTH_WEP:             return "wep";
        case WIFI_AUTH_WPA_PSK:         return "wpa";
        case WIFI_AUTH_WPA2_PSK:        return "wpa2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "wpa/wpa2";
        case WIFI_AUTH_WPA3_PSK:        return "wpa3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "wpa2/wpa3";
        case WIFI_AUTH_WAPI_PSK:        return "wapi";
        default:                        return "unknown";
    }
}

static void bounce_usb_link(void)
{
    /* Software equivalent of unplugging/replugging the USB cable, so
     * Windows (or any other OS) notices the "network" changed and
     * re-requests an IP via DHCP, instead of keeping its old lease.
     * Only takes effect if the project is built with CFG_TUD_NCM - the
     * ECM/RNDIS class doesn't use this same notification mechanism. */
    tud_network_link_state(0, false);
    vTaskDelay(pdMS_TO_TICKS(500));
    tud_network_link_state(0, true);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root, int http_status_ok /* 1=200 */)
{
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    if (!http_status_ok) {
        httpd_resp_set_status(req, "400 Bad Request");
    }
    esp_err_t ret;
    if (out) {
        ret = httpd_resp_sendstr(req, out);
        free(out);
    } else {
        ret = httpd_resp_sendstr(req, "{\"error\":\"internal\"}");
    }
    cJSON_Delete(root);
    return ret;
}

static esp_err_t send_error(httpd_req_t *req, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", msg);
    return send_json(req, root, 0);
}

/* ---------------------------------------------------------------------- */
/* GET /api/wifi/scan                                                     */
/* ---------------------------------------------------------------------- */

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    /* SmartConfig is disabled in usb_dongle_main.c now, but stop it
     * defensively anyway in case it's re-enabled later. */
    if (wifi_cmd_stop_smart_config() == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    wifi_scan_config_t scan_config = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time   = {
            .active = { .min = 200, .max = 500 },
        },
    };

    xEventGroupClearBits(s_scan_ev_grp, SCAN_DONE_BIT);

    /* Non-blocking start: we don't call esp_wifi_scan_get_ap_records()
     * ourselves at all (see the cache comment above) - we just wait for
     * scan_done_relay() to signal that cmd_wifi.c's scan_done_handler()
     * has already run and refreshed g_wifi_scan_cache for us. Since
     * scan_done_relay() is registered (in wifi_api_start()) after
     * scan_done_handler(), esp_event guarantees it fires second, so the
     * cache is always fresh by the time we read it below. */
    esp_err_t err = esp_wifi_scan_start(&scan_config, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start failed: %s", esp_err_to_name(err));
        return send_error(req, "scan_failed");
    }

    EventBits_t bits = xEventGroupWaitBits(s_scan_ev_grp, SCAN_DONE_BIT,
                                            pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(WIFI_API_SCAN_TIMEOUT_MS));
    if (!(bits & SCAN_DONE_BIT)) {
        return send_error(req, "scan_timeout");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "networks");

    /* Deduplicate by SSID, keep strongest RSSI, skip empty/hidden SSIDs. */
    for (int i = 0; i < g_wifi_scan_cache_count; i++) {
        if (g_wifi_scan_cache[i].ssid[0] == '\0') {
            continue;
        }
        bool dup = false;
        cJSON *existing;
        cJSON_ArrayForEach(existing, arr) {
            cJSON *ssid_item = cJSON_GetObjectItem(existing, "ssid");
            if (ssid_item && strcmp(ssid_item->valuestring, (char *)g_wifi_scan_cache[i].ssid) == 0) {
                cJSON *rssi_item = cJSON_GetObjectItem(existing, "rssi");
                if (rssi_item && g_wifi_scan_cache[i].rssi > rssi_item->valueint) {
                    cJSON_SetNumberValue(rssi_item, g_wifi_scan_cache[i].rssi);
                }
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        cJSON *net = cJSON_CreateObject();
        cJSON_AddStringToObject(net, "ssid", (char *)g_wifi_scan_cache[i].ssid);
        cJSON_AddNumberToObject(net, "rssi", g_wifi_scan_cache[i].rssi);
        cJSON_AddNumberToObject(net, "channel", g_wifi_scan_cache[i].primary);
        cJSON_AddStringToObject(net, "auth", authmode_to_str(g_wifi_scan_cache[i].authmode));
        cJSON_AddItemToArray(arr, net);
    }

    return send_json(req, root, 1);
}

/* ---------------------------------------------------------------------- */
/* GET /api/wifi/status                                                   */
/* ---------------------------------------------------------------------- */

static esp_err_t status_get_handler(httpd_req_t *req)
{
    /* esp_wifi_sta_get_ap_info() reflects the Wi-Fi driver's association
     * state directly - it works fine even though the ESP32 has no lwIP
     * IP address of its own on the STA side (see file header). */
    cJSON *root = cJSON_CreateObject();
    wifi_ap_record_t info;
    bool connected = (esp_wifi_sta_get_ap_info(&info) == ESP_OK);
    cJSON_AddBoolToObject(root, "connected", connected);
    if (connected) {
        cJSON_AddStringToObject(root, "ssid", (char *)info.ssid);
        cJSON_AddNumberToObject(root, "rssi", info.rssi);
        cJSON_AddStringToObject(root, "auth", authmode_to_str(info.authmode));
    }
    return send_json(req, root, 1);
}

/* ---------------------------------------------------------------------- */
/* POST /api/wifi/connect   body: {"ssid":"...","password":"..."}         */
/* ---------------------------------------------------------------------- */

static esp_err_t connect_post_handler(httpd_req_t *req)
{
    /* Same reasoning as in scan_get_handler() above. */
    wifi_cmd_stop_smart_config();

    if (req->content_len <= 0 || req->content_len >= WIFI_API_REQ_BODY_MAX) {
        return send_error(req, "invalid_body_size");
    }

    char buf[WIFI_API_REQ_BODY_MAX];
    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) {
        return send_error(req, "read_failed");
    }
    buf[received] = '\0';

    cJSON *body = cJSON_Parse(buf);
    if (!body) {
        return send_error(req, "invalid_json");
    }

    cJSON *ssid_item = cJSON_GetObjectItem(body, "ssid");
    cJSON *pass_item = cJSON_GetObjectItem(body, "password");

    if (!cJSON_IsString(ssid_item) || strlen(ssid_item->valuestring) == 0 ||
        strlen(ssid_item->valuestring) > 32) {
        cJSON_Delete(body);
        return send_error(req, "invalid_ssid");
    }
    const char *password = cJSON_IsString(pass_item) ? pass_item->valuestring : "";
    if (strlen(password) > 64) {
        cJSON_Delete(body);
        return send_error(req, "invalid_password");
    }

    /* Copy out of the cJSON object before we free it - wifi_cmd_sta_join()
     * is called after the HTTP response has been sent, by which point
     * `body` (and its strings) must already be gone. */
    char ssid_copy[33] = {0};
    char pass_copy[65] = {0};
    strlcpy(ssid_copy, ssid_item->valuestring, sizeof(ssid_copy));
    strlcpy(pass_copy, password, sizeof(pass_copy));
    cJSON_Delete(body);

    /* Ack immediately: wifi_cmd_sta_join() (called below) switches Wi-Fi
     * from APSTA back to STA-only mode as part of joining, which tears
     * down this very setup AP - and this HTTP connection along with it.
     * If we tried to wait for the join result before responding, the
     * browser would just see the connection drop with no response. */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "connecting");
    cJSON_AddStringToObject(root, "ssid", ssid_copy);
    cJSON_AddStringToObject(root, "note",
        "This setup network will disappear once the device switches to "
        "your Wi-Fi. Watch the device LED: green = connected, red = failed. "
        "If it fails, the setup AP does not currently come back "
        "automatically - power-cycle the device to try again.");
    esp_err_t ret = send_json(req, root, 1);

    vTaskDelay(pdMS_TO_TICKS(WIFI_API_POST_CONNECT_DELAY_MS));

    ESP_LOGI(TAG, "Joining '%s' via wifi_cmd_sta_join()...", ssid_copy);
    esp_err_t join_err = wifi_cmd_sta_join(ssid_copy, pass_copy);
    ESP_LOGI(TAG, "wifi_cmd_sta_join('%s') -> %s", ssid_copy, esp_err_to_name(join_err));

    if (join_err == ESP_OK) {
        /* We just switched from the setup AP/netif to bridging the real
         * network - bounce the USB link so the host re-requests DHCP
         * over the bridge instead of keeping its old 192.168.x lease. */
        bounce_usb_link();
    }

    return ret;
}

/* ---------------------------------------------------------------------- */
/* GET /  ->  minimal embedded web UI                                     */
/* ---------------------------------------------------------------------- */

static const char INDEX_HTML[] =
"<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
"<title>ESP32 Wi-Fi Setup</title><style>"
"body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 16px;background:#111;color:#eee}"
"h1{font-size:1.2rem}button{width:100%;padding:10px;margin:6px 0;border:0;border-radius:6px;"
"background:#2d7;color:#111;font-weight:bold;cursor:pointer}"
"input{width:100%;padding:8px;margin:4px 0;box-sizing:border-box;border-radius:6px;border:1px solid #444;"
"background:#1c1c1c;color:#eee}"
".ap{padding:8px;margin:4px 0;border:1px solid #333;border-radius:6px;cursor:pointer;display:flex;justify-content:space-between}"
".ap:hover{background:#1c1c1c}#status{margin-top:12px;font-size:.9rem;white-space:pre-wrap}"
"</style></head><body>"
"<h1>ESP32 USB Wi-Fi Dongle - Setup</h1>"
"<button onclick=\"scan()\">Scan networks</button>"
"<div id=\"list\"></div>"
"<div id=\"form\" style=\"display:none\">"
"<input id=\"ssid\" placeholder=\"SSID\">"
"<input id=\"pass\" type=\"password\" placeholder=\"Password\">"
"<button onclick=\"connect()\">Connect</button></div>"
"<div id=\"status\"></div>"
"<script>"
"function setStatus(t){document.getElementById('status').textContent=t}"
"async function scan(){setStatus('Scanning...');"
"const r=await fetch('/api/wifi/scan');const j=await r.json();"
"const list=document.getElementById('list');list.innerHTML='';"
"if(!j.networks||j.networks.length===0){setStatus('No networks found');return}"
"j.networks.sort((a,b)=>b.rssi-a.rssi);"
"j.networks.forEach(n=>{const d=document.createElement('div');d.className='ap';"
"d.innerHTML='<span>'+n.ssid+' ('+n.auth+')</span><span>'+n.rssi+' dBm</span>';"
"d.onclick=()=>{document.getElementById('ssid').value=n.ssid;"
"document.getElementById('form').style.display='block'};list.appendChild(d)});"
"setStatus('Found '+j.networks.length+' network(s)')}"
"async function connect(){const ssid=document.getElementById('ssid').value;"
"const pass=document.getElementById('pass').value;"
"if(!ssid){setStatus('Enter SSID');return}"
"setStatus('Connecting to '+ssid+'... this page will lose connection shortly, that is normal.');"
"try{const r=await fetch('/api/wifi/connect',{method:'POST',"
"headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({ssid:ssid,password:pass})});"
"const j=await r.json();setStatus(j.note||JSON.stringify(j))}"
"catch(e){setStatus('Device is switching networks - check its LED.')}}"
"</script></body></html>";

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

/* ---------------------------------------------------------------------- */
/* Public API                                                              */
/* ---------------------------------------------------------------------- */

/* --- reused from cmd_wifi.c (declared there without `static`) --------- */
extern esp_err_t wif_cmd_disconnect_wifi(void);

static esp_err_t enable_setup_ap(void)
{
    /* Bring up the setup Access Point alongside the STA bridge.
     * ap_netif already exists (created in initialise_wifi() via
     * esp_netif_create_default_wifi_ap()) with its default IP/DHCP
     * server (192.168.4.1/24) - we just need to enable the AP radio. */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(APSTA) failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_config_t ap_config = {
        .ap = {
            .ssid_len       = strlen(WIFI_API_AP_SSID),
            .channel        = WIFI_API_AP_CHANNEL,
            .max_connection = 4,
            .authmode       = (strlen(WIFI_API_AP_PASSWORD) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)ap_config.ap.ssid, WIFI_API_AP_SSID, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, WIFI_API_AP_PASSWORD, sizeof(ap_config.ap.password));

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(AP) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Setup AP '%s' up, config UI at http://192.168.4.1/", WIFI_API_AP_SSID);
    return ESP_OK;
}

esp_err_t wifi_api_start(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "wifi_api already started");
        return ESP_ERR_INVALID_STATE;
    }

    /* Must be called after initialise_wifi() (cmd_wifi.c) has already
     * registered its own WIFI_EVENT_SCAN_DONE handler - esp_event calls
     * handlers for the same event in registration order, so registering
     * ours here guarantees it always runs second, i.e. after the cache
     * (g_wifi_scan_cache) has been refreshed. See scan_get_handler(). */
    s_scan_ev_grp = xEventGroupCreate();
    if (!s_scan_ev_grp) {
        return ESP_ERR_NO_MEM;
    }
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                         &scan_done_relay, NULL, NULL);

    esp_err_t err = enable_setup_ap();
    if (err != ESP_OK) {
        return err;
    }

    httpd_config_t config    = HTTPD_DEFAULT_CONFIG();
    config.server_port       = 80;
    config.max_uri_handlers  = 8;
    config.lru_purge_enable  = true;
    config.stack_size        = 8192; /* scan + JSON handling needs headroom */

    err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t index_uri   = { .uri = "/",                 .method = HTTP_GET,  .handler = index_get_handler };
    static const httpd_uri_t scan_uri    = { .uri = "/api/wifi/scan",    .method = HTTP_GET,  .handler = scan_get_handler };
    static const httpd_uri_t status_uri  = { .uri = "/api/wifi/status",  .method = HTTP_GET,  .handler = status_get_handler };
    static const httpd_uri_t connect_uri = { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = connect_post_handler };

    httpd_register_uri_handler(s_server, &index_uri);
    httpd_register_uri_handler(s_server, &scan_uri);
    httpd_register_uri_handler(s_server, &status_uri);
    httpd_register_uri_handler(s_server, &connect_uri);

    return ESP_OK;
}

esp_err_t wifi_api_reset_for_reconfigure(void)
{
    ESP_LOGW(TAG, "Resetting Wi-Fi connection for reconfiguration");

    /* Cleanly disconnect - this correctly disables cmd_wifi.c's own
     * auto-reconnect (the `reconnect` flag) before disconnecting, so it
     * won't immediately reconnect back to the old network on its own.
     * Harmless no-op if we weren't connected to begin with. */
    wif_cmd_disconnect_wifi();

    /* s_wifi_is_connected (cmd_wifi.c) flips to false as part of the
     * disconnect above, which is all usb_net_recv_dispatch() needs to
     * start routing frames to the USB config netif again - no extra
     * action needed for http://192.168.7.1/ to come back.
     *
     * The Wi-Fi Setup AP, however, was forced back down to STA-only
     * mode by wifi_cmd_sta_join() when we originally connected, so it
     * needs to be explicitly re-enabled here. */
    esp_err_t err = enable_setup_ap();

    /* Same reasoning as in connect_post_handler(): the host still
     * thinks it's on the old (real Wi-Fi) network, so bounce the link
     * to make it re-request DHCP and pick up the USB config netif's
     * 192.168.7.1 again without needing a physical replug. */
    bounce_usb_link();

    return err;
}

esp_err_t wifi_api_stop(void)
{
    if (!s_server) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}

#else /* !(CFG_TUD_NCM || CFG_TUD_ECM_RNDIS) */

#include "esp_err.h"
#include "wifi_api.h"

esp_err_t wifi_api_start(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t wifi_api_stop(void)  { return ESP_ERR_NOT_SUPPORTED; }

#endif
