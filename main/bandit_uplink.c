#include "bandit_uplink.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
#define BANDIT_UPLINK_AP_CACHE_MAX 64
#define BANDIT_UPLINK_SESSION_LIST_MAX 32
#define BANDIT_UPLINK_DOWNLOAD_BUFFER 2048

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

static SemaphoreHandle_t s_ap_mutex;
static SemaphoreHandle_t s_radio_mutex;
static bandit_scan_record_t s_work_aps[BANDIT_UPLINK_AP_CACHE_MAX];
static size_t s_work_ap_count;
static size_t s_work_seen;
static bandit_scan_record_t s_published_aps[BANDIT_UPLINK_AP_CACHE_MAX];
static size_t s_published_ap_count;
static uint32_t s_published_generation;
static bool s_published_truncated;

static const char s_status_page[] =
    "<!doctype html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>C5 Bandit</title>"
    "<style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#090d12;color:#e2e8f0;"
    "max-width:60rem;margin:2rem auto;padding:0 1rem}"
    ".card{background:#151c25;border:1px solid #263241;border-radius:14px;padding:1rem;margin:.8rem 0}"
    "h1{margin:.1rem 0}.muted{color:#94a3b8}.grid{display:grid;grid-template-columns:1fr 1fr;gap:.7rem}"
    ".v{font-size:1.5rem;font-weight:700}code{color:#93c5fd}"
    ".scroll{overflow-x:auto}table{width:100%;border-collapse:collapse;font-size:.92rem}"
    "th,td{text-align:left;padding:.48rem .55rem;border-bottom:1px solid #263241;white-space:nowrap}"
    "th{color:#94a3b8;font-weight:600}td:first-child{max-width:18rem;overflow:hidden;text-overflow:ellipsis}"
    ".logs{display:flex;flex-wrap:wrap;gap:.55rem;margin-top:.7rem}"
    ".log{display:inline-block;padding:.55rem .7rem;border:1px solid #334155;border-radius:9px;"
    "color:#93c5fd;text-decoration:none}.log.active{border-color:#34d399;color:#6ee7b7}"
    "@media(max-width:520px){body{margin:1rem auto}.card{padding:.85rem}table{font-size:.82rem}}"
    "</style></head><body>"
    "<h1>C5 Bandit</h1><div class='muted' id='ver'>loading...</div>"
    "<div class='card'><b>Uplink</b><div id='net'>loading...</div></div>"
    "<div class='grid'>"
    "<div class='card'><div class='muted'>2.4 GHz</div><div class='v' id='g24'>--</div></div>"
    "<div class='card'><div class='muted'>5 GHz</div><div class='v' id='g5'>--</div></div>"
    "</div>"
    "<div class='card'><div class='muted'>Strongest</div><div class='v' id='strong'>--</div></div>"
    "<div class='card'><div id='summary'>waiting for scan...</div><div class='muted' id='storage'></div></div>"
    "<div class='card'><b>Latest scan APs</b><div class='muted' id='apsmeta'>waiting for scan...</div>"
    "<div class='scroll'><table><thead><tr><th>SSID</th><th>BSSID</th><th>Band</th><th>CH</th>"
    "<th>RSSI</th><th>Auth</th></tr></thead><tbody id='aps'></tbody></table></div></div>"
    "<div class='card'><b>TF sessions</b><div class='muted'>Newest 32 sessions; active log may lag by the flush interval.</div>"
    "<div class='logs' id='logs'><span class='muted'>loading...</span></div></div>"
    "<script>"
    "const $=id=>document.getElementById(id);"
    "function cell(tr,v){const d=document.createElement('td');d.textContent=v;tr.appendChild(d);}"
    "async function tick(){try{const r=await fetch('/status.json',{cache:'no-store'});const d=await r.json();"
    "$('ver').textContent=d.version;"
    "$('net').textContent=(d.connected?'Connected':'Disconnected')+' to '+d.ssid+' \\u00b7 '+(d.ip||'no IP');"
    "$('g24').textContent=d.ap24+' AP';$('g5').textContent=d.ap5+' AP';"
    "$('strong').textContent=d.strongest+' \\u00b7 '+d.rssi+' dBm \\u00b7 CH '+d.channel;"
    "$('summary').textContent='TOTAL '+d.total+' \\u00b7 OPEN '+d.open+' \\u00b7 HIDDEN '+d.hidden+' \\u00b7 SCAN #'+d.scan;"
    "$('storage').textContent='Storage: '+d.storage+' \\u00b7 uptime '+Math.floor(d.uptime_ms/1000)+' s';"
    "}catch(e){/* retain last-known status while HTTP server is busy */}}"
    "async function refreshAps(){try{const r=await fetch('/aps.json',{cache:'no-store'});const d=await r.json();"
    "const b=$('aps');b.replaceChildren();for(const a of d.aps){const tr=document.createElement('tr');"
    "cell(tr,a.ssid);cell(tr,a.bssid);cell(tr,a.band);cell(tr,a.channel);cell(tr,a.rssi+' dBm');cell(tr,a.auth);b.appendChild(tr);}"
    "$('apsmeta').textContent='Scan #'+d.scan+' \\u00b7 '+d.aps.length+' shown'+(d.truncated?' \\u00b7 strongest '+d.aps.length+' only':'');"
    "}catch(e){/* retain last complete AP table while HTTP server is busy */}}"
    "async function refreshLogs(){const box=$('logs');try{const r=await fetch('/logs.json',{cache:'no-store'});const d=await r.json();"
    "box.replaceChildren();if(!d.available||!d.sessions.length){const s=document.createElement('span');s.className='muted';"
    "s.textContent=d.available?'No sessions found.':'TF card unavailable.';box.appendChild(s);return;}"
    "for(const x of d.sessions){const a=document.createElement(x.active?'span':'a');a.className='log'+(x.active?' active':'');"
    "if(!x.active)a.href='/download?session='+x.index;a.textContent=x.name+' \\u00b7 '+Math.max(1,Math.round(x.bytes/1024))+' KiB'+(x.active?' \\u00b7 active':'');"
    "box.appendChild(a);}}catch(e){box.replaceChildren();const s=document.createElement('span');s.className='muted';"
    "s.textContent='Session list temporarily unavailable';box.appendChild(s);}}"
    "tick();refreshAps();refreshLogs();setInterval(tick,2000);setInterval(refreshAps,2000);setInterval(refreshLogs,10000);"
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

static int compare_ap_rssi_desc(const void *left, const void *right)
{
    const bandit_scan_record_t *a = left;
    const bandit_scan_record_t *b = right;

    if (a->rssi > b->rssi) {
        return -1;
    }
    if (a->rssi < b->rssi) {
        return 1;
    }
    return strcmp(a->ssid, b->ssid);
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
    httpd_resp_set_type(req, "text/html; charset=utf-8");
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

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_send(req, json, written);
    free(json);
    return ret;
}

static esp_err_t aps_json_handler(httpd_req_t *req)
{
    if (!s_ap_mutex) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "AP cache unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    bandit_scan_record_t *records = malloc(
        BANDIT_UPLINK_AP_CACHE_MAX * sizeof(*records)
    );
    if (!records) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;
    uint32_t generation = 0;
    bool truncated = false;

    if (xSemaphoreTake(s_ap_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        free(records);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "AP cache busy");
        return ESP_ERR_TIMEOUT;
    }

    count = s_published_ap_count;
    generation = s_published_generation;
    truncated = s_published_truncated;
    memcpy(records, s_published_aps, count * sizeof(*records));
    xSemaphoreGive(s_ap_mutex);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char chunk[384];
    int written = snprintf(
        chunk,
        sizeof(chunk),
        "{\"scan\":%lu,\"truncated\":%s,\"aps\":[",
        (unsigned long)generation,
        truncated ? "true" : "false"
    );
    if (written < 0 || written >= (int)sizeof(chunk)) {
        free(records);
        return ESP_FAIL;
    }

    esp_err_t ret = httpd_resp_send_chunk(req, chunk, written);
    for (size_t i = 0; ret == ESP_OK && i < count; i++) {
        const bandit_scan_record_t *record = &records[i];
        char escaped_ssid[70];
        char escaped_auth[30];
        char bssid[18];

        json_escape(record->ssid, escaped_ssid, sizeof(escaped_ssid));
        json_escape(record->auth, escaped_auth, sizeof(escaped_auth));
        snprintf(
            bssid,
            sizeof(bssid),
            "%02X:%02X:%02X:%02X:%02X:%02X",
            record->bssid[0],
            record->bssid[1],
            record->bssid[2],
            record->bssid[3],
            record->bssid[4],
            record->bssid[5]
        );

        written = snprintf(
            chunk,
            sizeof(chunk),
            "%s{\"ssid\":\"%s\",\"bssid\":\"%s\",\"rssi\":%d,"
            "\"channel\":%u,\"band\":\"%s\",\"auth\":\"%s\",\"hidden\":%s}",
            i ? "," : "",
            escaped_ssid,
            bssid,
            record->rssi,
            record->channel,
            record->channel > 14 ? "5 GHz" : "2.4 GHz",
            escaped_auth,
            record->hidden ? "true" : "false"
        );

        if (written < 0 || written >= (int)sizeof(chunk)) {
            ret = ESP_FAIL;
            break;
        }

        ret = httpd_resp_send_chunk(req, chunk, written);
    }

    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, "]}", 2);
    }
    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, NULL, 0);
    }

    free(records);
    return ret;
}

static esp_err_t logs_json_handler(httpd_req_t *req)
{
    bandit_storage_session_t *sessions = calloc(
        BANDIT_UPLINK_SESSION_LIST_MAX,
        sizeof(*sessions)
    );
    if (!sessions) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;
    esp_err_t list_ret = bandit_storage_list_sessions(
        sessions,
        BANDIT_UPLINK_SESSION_LIST_MAX,
        &count
    );

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (list_ret != ESP_OK) {
        free(sessions);
        return httpd_resp_sendstr(
            req,
            "{\"available\":false,\"sessions\":[]}"
        );
    }

    esp_err_t ret = httpd_resp_send_chunk(
        req,
        "{\"available\":true,\"sessions\":[",
        HTTPD_RESP_USE_STRLEN
    );

    char chunk[192];
    for (size_t i = 0; ret == ESP_OK && i < count; i++) {
        int written = snprintf(
            chunk,
            sizeof(chunk),
            "%s{\"index\":%u,\"name\":\"session-%04u.csv\","
            "\"bytes\":%zu,\"active\":%s}",
            i ? "," : "",
            sessions[i].index,
            sessions[i].index,
            sessions[i].size_bytes,
            sessions[i].active ? "true" : "false"
        );

        if (written < 0 || written >= (int)sizeof(chunk)) {
            ret = ESP_FAIL;
            break;
        }

        ret = httpd_resp_send_chunk(req, chunk, written);
    }

    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, "]}", 2);
    }
    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, NULL, 0);
    }

    free(sessions);
    return ret;
}

static esp_err_t send_all_raw(
    httpd_req_t *req,
    const char *data,
    size_t length
)
{
    while (length > 0) {
        int sent = httpd_send(req, data, length);
        if (sent <= 0) {
            return ESP_FAIL;
        }

        data += sent;
        length -= (size_t)sent;
    }

    return ESP_OK;
}

static esp_err_t download_handler(httpd_req_t *req)
{
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0 || query_len >= 64) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing session index");
        return ESP_ERR_INVALID_ARG;
    }

    char query[64];
    char value[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "session", value, sizeof(value)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid session query");
        return ESP_ERR_INVALID_ARG;
    }

    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed == 0 || parsed > 9999) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid session index");
        return ESP_ERR_INVALID_ARG;
    }

    char path[96];
    esp_err_t ret = bandit_storage_session_path(
        (unsigned)parsed,
        path,
        sizeof(path)
    );
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid session path");
        return ret;
    }

    const char *active_path = bandit_storage_get_session_path();
    if (active_path && active_path[0] && strcmp(path, active_path) == 0) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Active session is still being written");
    }

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Session not found");
        return ESP_ERR_NOT_FOUND;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Session not found");
        return ESP_ERR_NOT_FOUND;
    }

    char *buffer = malloc(BANDIT_UPLINK_DOWNLOAD_BUFFER);
    if (!buffer) {
        fclose(file);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    // Wait for any active RF sweep to finish, then hold the radio on the STA's
    // home channel for the duration of the completed-session transfer.
    if (!s_radio_mutex ||
        xSemaphoreTake(s_radio_mutex, pdMS_TO_TICKS(15000)) != pdTRUE) {
        free(buffer);
        fclose(file);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Radio busy; try the download again");
    }

    // ESP-IDF's chunked response helper necessarily emits
    // Transfer-Encoding: chunked. Mobile download managers may report that
    // as an unknown/-1 byte attachment. Completed CSVs already have a stable
    // length, so frame the response explicitly with Content-Length while
    // still streaming the file in small chunks from TF.
    char header[384];
    int header_len = snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/csv; charset=utf-8\r\n"
        "Content-Length: %lld\r\n"
        "Content-Disposition: attachment; filename=\"session-%04lu.csv\"\r\n"
        "Cache-Control: no-store\r\n"
        "\r\n",
        (long long)st.st_size,
        parsed
    );

    if (header_len <= 0 || header_len >= (int)sizeof(header)) {
        xSemaphoreGive(s_radio_mutex);
        free(buffer);
        fclose(file);
        return ESP_FAIL;
    }

    ret = send_all_raw(req, header, (size_t)header_len);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_radio_mutex);
        free(buffer);
        fclose(file);
        return ret;
    }

    size_t total_sent = 0;
    while (total_sent < (size_t)st.st_size) {
        size_t remaining = (size_t)st.st_size - total_sent;
        size_t request = remaining < BANDIT_UPLINK_DOWNLOAD_BUFFER
            ? remaining
            : BANDIT_UPLINK_DOWNLOAD_BUFFER;

        size_t read_count = fread(buffer, 1, request, file);
        if (read_count == 0) {
            ESP_LOGW(TAG, "short read while downloading %s", path);
            ret = ESP_FAIL;
            break;
        }

        ret = send_all_raw(req, buffer, read_count);
        if (ret != ESP_OK) {
            break;
        }

        total_sent += read_count;
    }

    if (ret == ESP_OK && total_sent != (size_t)st.st_size) {
        ESP_LOGW(
            TAG,
            "download size mismatch for %s: expected=%lld sent=%zu",
            path,
            (long long)st.st_size,
            total_sent
        );
        ret = ESP_FAIL;
    }

    xSemaphoreGive(s_radio_mutex);
    free(buffer);
    fclose(file);
    return ret;
}

static esp_err_t start_status_server(void)
{
    if (s_server) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = BANDIT_UPLINK_HTTP_STACK;
    config.max_uri_handlers = 5;

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
    httpd_uri_t aps = {
        .uri = "/aps.json",
        .method = HTTP_GET,
        .handler = aps_json_handler,
    };
    httpd_uri_t logs = {
        .uri = "/logs.json",
        .method = HTTP_GET,
        .handler = logs_json_handler,
    };
    httpd_uri_t download = {
        .uri = "/download",
        .method = HTTP_GET,
        .handler = download_handler,
    };

    ret = httpd_register_uri_handler(s_server, &root);
    if (ret == ESP_OK) {
        ret = httpd_register_uri_handler(s_server, &status);
    }
    if (ret == ESP_OK) {
        ret = httpd_register_uri_handler(s_server, &aps);
    }
    if (ret == ESP_OK) {
        ret = httpd_register_uri_handler(s_server, &logs);
    }
    if (ret == ESP_OK) {
        ret = httpd_register_uri_handler(s_server, &download);
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
        bandit_ui_set_uplink_state(true, false);
        bandit_ui_set_uplink_address(NULL);
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

    bandit_ui_set_uplink_state(true, true);
    bandit_ui_set_uplink_address(ip);
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
    if (!s_ap_mutex) {
        s_ap_mutex = xSemaphoreCreateMutex();
        if (!s_ap_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_radio_mutex) {
        s_radio_mutex = xSemaphoreCreateMutex();
        if (!s_radio_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    char password[64] = {0};
    char ssid[33] = {0};

    esp_err_t ret = load_credentials(ssid, password);
    if (ret == ESP_ERR_NVS_NOT_FOUND || (ret == ESP_OK && ssid[0] == '\0')) {
        bandit_ui_set_uplink_state(false, false);
        ESP_LOGI(TAG, "uplink not configured");
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "unable to load uplink credentials: %s", esp_err_to_name(ret));
        return ret;
    }

    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, strlen(ssid));
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
    bandit_ui_set_uplink_state(true, false);

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

    bandit_ui_set_uplink_state(false, false);
    bandit_ui_set_uplink_address(NULL);

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

void bandit_uplink_begin_scan(void)
{
    if (s_radio_mutex) {
        xSemaphoreTake(s_radio_mutex, portMAX_DELAY);
    }

    s_work_ap_count = 0;
    s_work_seen = 0;
}

void bandit_uplink_end_scan(void)
{
    if (s_radio_mutex) {
        xSemaphoreGive(s_radio_mutex);
    }
}

void bandit_uplink_observe_record(const bandit_scan_record_t *record)
{
    if (!record) {
        return;
    }

    s_work_seen++;

    if (s_work_ap_count < BANDIT_UPLINK_AP_CACHE_MAX) {
        s_work_aps[s_work_ap_count++] = *record;
        return;
    }

    // Keep the strongest bounded set if a dense environment exceeds the
    // browser cache capacity.
    size_t weakest = 0;
    for (size_t i = 1; i < s_work_ap_count; i++) {
        if (s_work_aps[i].rssi < s_work_aps[weakest].rssi) {
            weakest = i;
        }
    }

    if (record->rssi > s_work_aps[weakest].rssi) {
        s_work_aps[weakest] = *record;
    }
}

void bandit_uplink_publish_snapshot(const bandit_scan_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }

    qsort(
        s_work_aps,
        s_work_ap_count,
        sizeof(s_work_aps[0]),
        compare_ap_rssi_desc
    );

    if (s_ap_mutex &&
        xSemaphoreTake(s_ap_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        s_published_ap_count = s_work_ap_count;
        s_published_generation = snapshot->generation;
        s_published_truncated = s_work_seen > s_work_ap_count;
        memcpy(
            s_published_aps,
            s_work_aps,
            s_work_ap_count * sizeof(s_work_aps[0])
        );
        xSemaphoreGive(s_ap_mutex);
    }

    portENTER_CRITICAL(&s_state_mux);
    s_snapshot = *snapshot;
    s_have_snapshot = true;
    portEXIT_CRITICAL(&s_state_mux);
}
