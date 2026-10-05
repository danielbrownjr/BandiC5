#include "bandit_uplink.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "mdns.h"

#include "bandit_storage.h"
#include "bandit_ui.h"
#include "bandit_version.h"

#define BANDIT_UPLINK_NVS_NAMESPACE "uplink"
#define BANDIT_UPLINK_NVS_SSID "ssid"
#define BANDIT_UPLINK_NVS_PASSWORD "password"
#define BANDIT_UPLINK_RETRY_MS 15000
#define BANDIT_UPLINK_HTTP_STACK 8192

static const char *TAG = "bandit_uplink";

static volatile bool s_enabled;
static bool s_connected;
static char s_ssid[33];
static char s_ip[16];
static httpd_handle_t s_server;
static int64_t s_next_retry_ms;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static bool s_wifi_handler_registered;
static bool s_ip_handler_registered;
static bool s_mdns_started;

static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static bandit_scan_snapshot_t s_snapshot;
static bool s_have_snapshot;

static const char s_status_page[] =
    "<!doctype html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>C5 Bandit</title>"
    "<style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#090d12;color:#e2e8f0;"
    "max-width:42rem;margin:2rem auto;padding:0 1rem}"
    ".card{background:#151c25;border:1px solid #263241;border-radius:14px;padding:1rem;margin:.8rem 0}"
    "h1{margin:.1rem 0}.muted{color:#94a3b8}.grid{display:grid;grid-template-columns:1fr 1fr;gap:.7rem}"
    ".v{font-size:1.5rem;font-weight:700}code{color:#93c5fd}"
    "</style></head><body>"
    "<h1>C5 Bandit</h1><div class='muted' id='ver'>loading...</div>"
    "<div class='card'><b>Uplink</b><div id='net'>loading...</div></div>"
    "<div class='grid'>"
    "<div class='card'><div class='muted'>2.4 GHz</div><div class='v' id='g24'>--</div></div>"
    "<div class='card'><div class='muted'>5 GHz</div><div class='v' id='g5'>--</div></div>"
    "</div>"
    "<div class='card'><div class='muted'>Strongest</div><div class='v' id='strong'>--</div></div>"
    "<div class='card'><div id='summary'>waiting for scan...</div><div class='muted' id='storage'></div></div>"
    "<script>"
    "const $=id=>document.getElementById(id);"
    "async function tick(){try{const r=await fetch('/status.json',{cache:'no-store'});"
    "const d=await r.json();"
    "$('ver').textContent=d.version;"
    "$('net').textContent=(d.connected?'Connected':'Disconnected')+' to '+d.ssid+' · '+(d.ip||'no IP');"
    "$('g24').textContent=d.ap24+' AP';$('g5').textContent=d.ap5+' AP';"
    "$('strong').textContent=d.strongest+' · '+d.rssi+' dBm · CH '+d.channel;"
    "$('summary').textContent='TOTAL '+d.total+' · OPEN '+d.open+' · HIDDEN '+d.hidden+' · SCAN #'+d.scan;"
    "$('storage').textContent='Storage: '+d.storage+' · uptime '+Math.floor(d.uptime_ms/1000)+' s';"
    "}catch(e){$('net').textContent='Status temporarily unavailable';}}"
    "tick();setInterval(tick,2000);"
    "</script></body></html>";

static void json_escape(const char *src, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return;
    }

    size_t out = 0;
    const char *p = src ? src : "";

    while (*p && out + 1 < dst_size) {
        unsigned char ch = (unsigned char)*p++;

        if (ch == '"' || ch == '\\') {
            if (out + 2 >= dst_size) {
                break;
            }
            dst[out++] = '\\';
            dst[out++] = (char)ch;
        } else if (ch >= 0x20) {
            dst[out++] = (char)ch;
        }
    }

    dst[out] = '\0';
}

static const char *storage_state_name(void)
{
    switch (bandit_storage_get_state()) {
    case BANDIT_STORAGE_READY:
        return "logging";
    case BANDIT_STORAGE_ERROR:
        return "error";
    case BANDIT_STORAGE_NO_CARD:
    default:
        return "no card";
    }
}

static esp_err_t status_root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, s_status_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_json_handler(httpd_req_t *req)
{
    bandit_scan_snapshot_t snapshot = {0};
    bool have_snapshot;
    bool connected;
    char ssid[33];
    char ip[16];

    portENTER_CRITICAL(&s_state_mux);
    snapshot = s_snapshot;
    have_snapshot = s_have_snapshot;
    connected = s_connected;
    snprintf(ssid, sizeof(ssid), "%s", s_ssid);
    snprintf(ip, sizeof(ip), "%s", s_ip);
    portEXIT_CRITICAL(&s_state_mux);

    char escaped_ssid[70];
    char escaped_strongest[70];
    char escaped_auth[30];
    json_escape(ssid, escaped_ssid, sizeof(escaped_ssid));
    json_escape(snapshot.strongest_ssid, escaped_strongest, sizeof(escaped_strongest));
    json_escape(snapshot.strongest_auth, escaped_auth, sizeof(escaped_auth));

    char *json = malloc(768);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    int written = snprintf(
        json,
        768,
        "{"
        "\"version\":\"%s\","
        "\"connected\":%s,"
        "\"ssid\":\"%s\","
        "\"ip\":\"%s\","
        "\"uptime_ms\":%lld,"
        "\"scan\":%lu,"
        "\"total\":%u,"
        "\"ap24\":%u,"
        "\"ap5\":%u,"
        "\"open\":%u,"
        "\"hidden\":%u,"
        "\"strongest\":\"%s\","
        "\"rssi\":%d,"
        "\"channel\":%u,"
        "\"auth\":\"%s\","
        "\"storage\":\"%s\""
        "}",
        BANDIT_VERSION,
        connected ? "true" : "false",
        escaped_ssid,
        ip,
        (long long)(esp_timer_get_time() / 1000),
        have_snapshot ? (unsigned long)snapshot.generation : 0UL,
        have_snapshot ? snapshot.total : 0U,
        have_snapshot ? snapshot.count_2g : 0U,
        have_snapshot ? snapshot.count_5g : 0U,
        have_snapshot ? snapshot.open : 0U,
        have_snapshot ? snapshot.hidden : 0U,
        have_snapshot ? escaped_strongest : "none",
        have_snapshot ? snapshot.strongest_rssi : -127,
        have_snapshot ? snapshot.strongest_channel : 0U,
        have_snapshot ? escaped_auth : "--",
        storage_state_name()
    );

    if (written < 0 || written >= 768) {
        free(json);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Status too large");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_send(req, json, written);
    free(json);
    return ret;
}

static esp_err_t start_status_server(void)
{
    if (s_server) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = BANDIT_UPLINK_HTTP_STACK;
    config.max_uri_handlers = 3;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "status server start failed: %s", esp_err_to_name(ret));
        s_server = NULL;
        return ret;
    }

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = status_root_handler,
    };
    httpd_uri_t status = {
        .uri = "/status.json",
        .method = HTTP_GET,
        .handler = status_json_handler,
    };

    ret = httpd_register_uri_handler(s_server, &root);
    if (ret == ESP_OK) {
        ret = httpd_register_uri_handler(s_server, &status);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "status route registration failed: %s", esp_err_to_name(ret));
        httpd_stop(s_server);
        s_server = NULL;
        return ret;
    }

    return ESP_OK;
}

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    if (!s_enabled) {
        return;
    }

    if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        portENTER_CRITICAL(&s_state_mux);
        s_connected = false;
        s_ip[0] = '\0';
        portEXIT_CRITICAL(&s_state_mux);
        bandit_ui_set_uplink_state(false);
        ESP_LOGW(TAG, "uplink disconnected; reconnect scheduled between scans");
    }
}

static void ip_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)arg;
    (void)event_base;

    if (!s_enabled || event_id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));

    portENTER_CRITICAL(&s_state_mux);
    s_connected = true;
    snprintf(s_ip, sizeof(s_ip), "%s", ip);
    portEXIT_CRITICAL(&s_state_mux);

    bandit_ui_set_uplink_state(true);
    ESP_LOGI(TAG, "uplink connected: SSID=%s IP=%s hostname=bandic5", s_ssid, ip);

    esp_err_t ret = start_status_server();
    if (ret == ESP_OK) {
        if (!s_mdns_started) {
            esp_err_t mdns_ret = mdns_init();
            if (mdns_ret == ESP_OK) {
                (void)mdns_hostname_set("bandic5");
                (void)mdns_instance_name_set("C5 Bandit");
                (void)mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
                s_mdns_started = true;
            } else {
                ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(mdns_ret));
            }
        }

        ESP_LOGI(TAG, "status page ready: http://%s/ (try http://bandic5.local/)", ip);
    }
}

static esp_err_t load_credentials(char ssid[33], char password[64])
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(BANDIT_UPLINK_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t ssid_len = 33;
    size_t password_len = 64;

    ret = nvs_get_str(handle, BANDIT_UPLINK_NVS_SSID, ssid, &ssid_len);
    if (ret == ESP_OK) {
        ret = nvs_get_str(handle, BANDIT_UPLINK_NVS_PASSWORD, password, &password_len);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            password[0] = '\0';
            ret = ESP_OK;
        }
    }

    nvs_close(handle);
    return ret;
}

bool bandit_uplink_get_configured_ssid(char out_ssid[33])
{
    if (!out_ssid) {
        return false;
    }

    char password[64];
    esp_err_t ret = load_credentials(out_ssid, password);
    return ret == ESP_OK && out_ssid[0] != '\0';
}

esp_err_t bandit_uplink_save_credentials(const char *ssid, const char *password)
{
    if (!ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ssid_len = strlen(ssid);
    size_t password_len = strlen(password);
    if (ssid_len == 0) {
        return bandit_uplink_clear_credentials();
    }
    if (ssid_len > 32 || password_len > 63) {
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(BANDIT_UPLINK_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(handle, BANDIT_UPLINK_NVS_SSID, ssid);
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, BANDIT_UPLINK_NVS_PASSWORD, password);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
}

esp_err_t bandit_uplink_clear_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(BANDIT_UPLINK_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_erase_all(handle);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
}

esp_err_t bandit_uplink_init(void)
{
    char password[64] = {0};
    char ssid[33] = {0};

    esp_err_t ret = load_credentials(ssid, password);
    if (ret == ESP_ERR_NVS_NOT_FOUND || (ret == ESP_OK && ssid[0] == '\0')) {
        ESP_LOGI(TAG, "uplink not configured");
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "unable to load uplink credentials: %s", esp_err_to_name(ret));
        return ret;
    }

    wifi_config_t config = {0};
    snprintf((char *)config.sta.ssid, sizeof(config.sta.ssid), "%s", ssid);
    snprintf((char *)config.sta.password, sizeof(config.sta.password), "%s", password);
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_err_t hostname_ret = esp_netif_set_hostname(sta, "bandic5");
        if (hostname_ret != ESP_OK) {
            ESP_LOGW(TAG, "hostname set failed: %s", esp_err_to_name(hostname_ret));
        }
    }

    ret = esp_event_handler_instance_register(
        WIFI_EVENT,
        WIFI_EVENT_STA_DISCONNECTED,
        wifi_event_handler,
        NULL,
        &s_wifi_handler
    );
    if (ret != ESP_OK) {
        return ret;
    }
    s_wifi_handler_registered = true;

    ret = esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        ip_event_handler,
        NULL,
        &s_ip_handler
    );
    if (ret != ESP_OK) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, s_wifi_handler);
        s_wifi_handler_registered = false;
        return ret;
    }
    s_ip_handler_registered = true;

    ret = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (ret != ESP_OK) {
        bandit_uplink_stop();
        return ret;
    }

    portENTER_CRITICAL(&s_state_mux);
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    s_ip[0] = '\0';
    s_connected = false;
    portEXIT_CRITICAL(&s_state_mux);

    s_enabled = true;

    s_next_retry_ms = 0;

    ESP_LOGI(TAG, "uplink enabled for SSID=%s; first connect follows initial RF scan", s_ssid);
    return ESP_OK;
}

void bandit_uplink_service(void)
{
    if (!s_enabled) {
        return;
    }

    bool connected;
    portENTER_CRITICAL(&s_state_mux);
    connected = s_connected;
    portEXIT_CRITICAL(&s_state_mux);

    if (connected) {
        return;
    }

    const int64_t current_ms = esp_timer_get_time() / 1000;
    if (s_next_retry_ms != 0 && current_ms < s_next_retry_ms) {
        return;
    }

    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN && ret != ESP_ERR_WIFI_STATE) {
        ESP_LOGD(TAG, "uplink connect attempt: %s", esp_err_to_name(ret));
    }

    s_next_retry_ms = current_ms + BANDIT_UPLINK_RETRY_MS;
}

void bandit_uplink_stop(void)
{
    s_enabled = false;
    s_next_retry_ms = 0;

    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }

    if (s_mdns_started) {
        mdns_free();
        s_mdns_started = false;
    }

    bandit_ui_set_uplink_state(false);

    if (s_wifi_handler_registered) {
        esp_event_handler_instance_unregister(
            WIFI_EVENT,
            WIFI_EVENT_STA_DISCONNECTED,
            s_wifi_handler
        );
        s_wifi_handler_registered = false;
    }

    if (s_ip_handler_registered) {
        esp_event_handler_instance_unregister(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            s_ip_handler
        );
        s_ip_handler_registered = false;
    }

    portENTER_CRITICAL(&s_state_mux);
    s_connected = false;
    s_ip[0] = '\0';
    portEXIT_CRITICAL(&s_state_mux);
}

void bandit_uplink_publish_snapshot(const bandit_scan_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }

    portENTER_CRITICAL(&s_state_mux);
    s_snapshot = *snapshot;
    s_have_snapshot = true;
    portEXIT_CRITICAL(&s_state_mux);
}
