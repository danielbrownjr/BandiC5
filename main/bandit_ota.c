#include "bandit_ota.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "bandit_ui.h"
#include "bandit_uplink.h"
#include "bandit_version.h"

#define BANDIT_OTA_BOOT_GPIO GPIO_NUM_28
#define BANDIT_OTA_HOLD_MS 2000
#define BANDIT_OTA_POLL_MS 100
#define BANDIT_OTA_BUTTON_TASK_STACK 3072
#define BANDIT_OTA_RECV_BUFFER 4096
#define BANDIT_OTA_MAX_RECV_TIMEOUTS 6
#define BANDIT_OTA_REBOOT_DELAY_MS 4000

static const char *TAG = "bandit_ota";

static volatile bool s_ota_requested;
static httpd_handle_t s_server;
static esp_netif_t *s_ap_netif;

static const char s_update_page[] =
    "<!doctype html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>BandiC5 Update</title>"
    "<style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#090d12;color:#e2e8f0;"
    "max-width:34rem;margin:3rem auto;padding:0 1.2rem}"
    ".card{background:#151c25;border:1px solid #263241;border-radius:14px;padding:1.2rem}"
    "h1{margin-top:0}input,button{font:inherit;width:100%;box-sizing:border-box;margin:.6rem 0;"
    "padding:.9rem;border-radius:10px;border:1px solid #334155}"
    "button{background:#34d399;color:#06110d;font-weight:700}"
    "progress{width:100%;height:1.2rem}#status{min-height:1.5rem;color:#94a3b8}"
    "code{color:#93c5fd}"
    "</style></head><body><div class='card'>"
    "<h1>C5 Bandit Update</h1>"
    "<p>Running: <code>" BANDIT_VERSION "</code></p>"
    "<p>Select a <code>bandic5.bin</code> firmware image.</p>"
    "<input id='file' type='file' accept='.bin,application/octet-stream'>"
    "<button id='go'>Upload firmware</button>"
    "<progress id='bar' max='100' value='0'></progress>"
    "<p id='status'>Waiting for firmware.</p>"
    "</div>"
    "<div class='card'><h2>Uplink Wi-Fi</h2>"
    "<p>Join a hotspot/router during normal scouting for remote read-only monitoring.</p>"
    "<form method='POST' action='/wifi'>"
    "<input name='ssid' maxlength='32' placeholder='SSID'>"
    "<input name='password' type='password' maxlength='63' placeholder='Password (blank for open Wi-Fi)'>"
    "<button type='submit'>Save Wi-Fi &amp; reboot</button>"
    "</form><p style='color:#94a3b8'>Leave SSID blank to disable uplink.</p></div>"
    "<script>"
    "const f=document.getElementById('file'),s=document.getElementById('status'),"
    "b=document.getElementById('bar'),g=document.getElementById('go');"
    "g.onclick=()=>{"
    "if(!f.files.length){s.textContent='Choose bandic5.bin first.';return;}"
    "g.disabled=true;b.value=0;s.textContent='Uploading...';"
    "const x=new XMLHttpRequest();let sentAll=false;"
    "x.open('POST','/update');x.setRequestHeader('Content-Type','application/octet-stream');"
    "x.upload.onprogress=e=>{if(e.lengthComputable){b.value=Math.round(e.loaded*100/e.total);"
    "sentAll=e.loaded===e.total;s.textContent='Uploading... '+b.value+'%';}};"
    "x.onload=()=>{if(x.status>=200&&x.status<300){b.value=100;"
    "s.textContent='Update accepted. C5 Bandit is rebooting...';}"
    "else{s.textContent='Update failed: '+(x.responseText||('HTTP '+x.status));g.disabled=false;}};"
    "x.onerror=()=>{if(sentAll){b.value=100;"
    "s.textContent='Upload sent. Connection closed for reboot; check C5 Bandit.';}"
    "else{b.value=0;s.textContent='Upload failed before completion.';g.disabled=false;}};"
    "x.ontimeout=()=>{if(sentAll){b.value=100;"
    "s.textContent='Upload sent. Device may be rebooting.';}"
    "else{b.value=0;s.textContent='Upload timed out.';g.disabled=false;}};"
    "x.timeout=120000;x.send(f.files[0]);};"
    "</script></body></html>";

static void ota_button_task(void *arg)
{
    (void)arg;

    unsigned held_ms = 0;

    while (!s_ota_requested) {
        if (gpio_get_level(BANDIT_OTA_BOOT_GPIO) == 0) {
            held_ms += BANDIT_OTA_POLL_MS;
            if (held_ms >= BANDIT_OTA_HOLD_MS) {
                s_ota_requested = true;
                ESP_LOGI(TAG, "OTA requested by BOOT button");
                break;
            }
        } else {
            held_ms = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(BANDIT_OTA_POLL_MS));
    }

    vTaskDelete(NULL);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    ESP_LOGI(
        TAG,
        "HTTP root request, stack high water=%u",
        (unsigned)uxTaskGetStackHighWaterMark(NULL)
    );
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, s_update_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t probe_get_handler(httpd_req_t *req)
{
    ESP_LOGI(
        TAG,
        "HTTP probe %s, stack high water=%u",
        req->uri,
        (unsigned)uxTaskGetStackHighWaterMark(NULL)
    );
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, "", 0);
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static void url_decode(const char *src, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return;
    }

    size_t out = 0;
    const char *p = src ? src : "";

    while (*p && out + 1 < dst_size) {
        if (*p == '+') {
            dst[out++] = ' ';
            p++;
            continue;
        }

        if (*p == '%' && p[1] && p[2]) {
            int hi = hex_value(p[1]);
            int lo = hex_value(p[2]);
            if (hi >= 0 && lo >= 0) {
                dst[out++] = (char)((hi << 4) | lo);
                p += 3;
                continue;
            }
        }

        dst[out++] = *p++;
    }

    dst[out] = '\0';
}

static bool form_value(
    const char *body,
    const char *key,
    char *decoded,
    size_t decoded_size
)
{
    if (!body || !key || !decoded || decoded_size == 0) {
        return false;
    }

    size_t key_len = strlen(key);
    const char *p = body;

    while (*p) {
        const char *entry_end = strchr(p, '&');
        if (!entry_end) {
            entry_end = p + strlen(p);
        }

        const char *equals = memchr(p, '=', (size_t)(entry_end - p));
        if (equals && (size_t)(equals - p) == key_len && strncmp(p, key, key_len) == 0) {
            size_t encoded_len = (size_t)(entry_end - equals - 1);
            char encoded[256];

            if (encoded_len >= sizeof(encoded)) {
                return false;
            }

            memcpy(encoded, equals + 1, encoded_len);
            encoded[encoded_len] = '\0';
            url_decode(encoded, decoded, decoded_size);
            return true;
        }

        p = *entry_end ? entry_end + 1 : entry_end;
    }

    return false;
}

static esp_err_t wifi_config_post_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= 384) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Wi-Fi form");
        return ESP_FAIL;
    }

    char body[384];
    size_t remaining = req->content_len;
    size_t offset = 0;
    unsigned timeout_count = 0;

    while (remaining > 0) {
        int received = httpd_req_recv(req, body + offset, remaining);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            timeout_count++;
            if (timeout_count >= BANDIT_OTA_MAX_RECV_TIMEOUTS) {
                httpd_resp_set_status(req, "408 Request Timeout");
                return httpd_resp_sendstr(req, "Wi-Fi form receive timed out");
            }
            continue;
        }
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Form receive failed");
            return ESP_FAIL;
        }

        timeout_count = 0;
        offset += (size_t)received;
        remaining -= (size_t)received;
    }
    body[offset] = '\0';

    char ssid[33] = {0};
    char password[64] = {0};

    if (!form_value(body, "ssid", ssid, sizeof(ssid))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID missing");
        return ESP_FAIL;
    }
    (void)form_value(body, "password", password, sizeof(password));

    esp_err_t ret = ssid[0]
        ? bandit_uplink_save_credentials(ssid, password)
        : bandit_uplink_clear_credentials();

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uplink credential save failed: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to save Wi-Fi");
        return ret;
    }

    ESP_LOGI(TAG, "uplink configuration %s", ssid[0] ? "saved" : "cleared");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(
        req,
        "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<body style='font-family:sans-serif;background:#090d12;color:#e2e8f0;padding:2rem'>"
        "<h2>Wi-Fi saved</h2><p>C5 Bandit is rebooting into scout mode.</p></body>"
    );

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t update_post_handler(httpd_req_t *req)
{
    ESP_LOGI(
        TAG,
        "HTTP OTA upload start, stack high water=%u",
        (unsigned)uxTaskGetStackHighWaterMark(NULL)
    );

    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty firmware image");
        return ESP_FAIL;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA slot available");
        return ESP_FAIL;
    }

    if ((size_t)req->content_len > update_partition->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware image is too large");
        return ESP_FAIL;
    }

    ESP_LOGI(
        TAG,
        "OTA upload: %zu bytes -> %s @ 0x%08lx",
        req->content_len,
        update_partition->label,
        (unsigned long)update_partition->address
    );

    char *buffer = malloc(BANDIT_OTA_RECV_BUFFER);
    if (!buffer) {
        ESP_LOGE(TAG, "unable to allocate %d-byte OTA receive buffer", BANDIT_OTA_RECV_BUFFER);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t ret = esp_ota_begin(update_partition, req->content_len, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(ret));
        free(buffer);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to start OTA write");
        return ret;
    }

    size_t remaining = req->content_len;
    size_t received_total = 0;
    int last_percent = -1;
    unsigned timeout_count = 0;

    while (remaining > 0) {
        size_t to_read = remaining < BANDIT_OTA_RECV_BUFFER
            ? remaining
            : BANDIT_OTA_RECV_BUFFER;
        int received = httpd_req_recv(req, buffer, to_read);

        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            timeout_count++;
            if (timeout_count >= BANDIT_OTA_MAX_RECV_TIMEOUTS) {
                ESP_LOGE(TAG, "firmware upload timed out");
                esp_ota_abort(ota_handle);
                free(buffer);
                bandit_ui_set_ota_progress(0, "UPLOAD TIMEOUT");
                httpd_resp_set_status(req, "408 Request Timeout");
                return httpd_resp_sendstr(req, "Firmware upload timed out");
            }
            continue;
        }

        if (received <= 0) {
            ESP_LOGE(TAG, "firmware upload interrupted");
            esp_ota_abort(ota_handle);
            free(buffer);
            bandit_ui_set_ota_progress(0, "UPLOAD FAILED");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload interrupted");
            return ESP_FAIL;
        }

        timeout_count = 0;

        ret = esp_ota_write(ota_handle, buffer, received);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(ret));
            esp_ota_abort(ota_handle);
            free(buffer);
            bandit_ui_set_ota_progress(0, "WRITE FAILED");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash write failed");
            return ret;
        }

        received_total += received;
        remaining -= received;

        int percent = (int)((received_total * 100U) / req->content_len);
        if (percent != last_percent && (percent == 100 || percent - last_percent >= 5)) {
            bandit_ui_set_ota_progress(percent, "UPLOADING");
            last_percent = percent;
        }
    }

    free(buffer);
    buffer = NULL;

    ret = esp_ota_end(ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(ret));
        bandit_ui_set_ota_progress(0, "INVALID IMAGE");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware validation failed");
        return ret;
    }

    ret = esp_ota_set_boot_partition(update_partition);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set boot partition failed: %s", esp_err_to_name(ret));
        bandit_ui_set_ota_progress(0, "BOOT SET FAILED");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to activate firmware");
        return ret;
    }

    bandit_ui_set_ota_progress(100, "REBOOTING");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "Update accepted. Rebooting C5 Bandit.");

    ESP_LOGI(
        TAG,
        "OTA complete; rebooting into %s after %d ms",
        update_partition->label,
        BANDIT_OTA_REBOOT_DELAY_MS
    );
    vTaskDelay(pdMS_TO_TICKS(BANDIT_OTA_REBOOT_DELAY_MS));
    esp_restart();

    return ESP_OK;
}

esp_err_t bandit_ota_button_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BANDIT_OTA_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        return ret;
    }

    s_ota_requested = false;

    BaseType_t task_ret = xTaskCreate(
        ota_button_task,
        "ota_button",
        BANDIT_OTA_BUTTON_TASK_STACK,
        NULL,
        4,
        NULL
    );

    return task_ret == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool bandit_ota_requested(void)
{
    return s_ota_requested;
}

esp_err_t bandit_ota_start(void)
{
    bandit_ui_show_ota_mode(
        BANDIT_OTA_AP_SSID,
        BANDIT_OTA_AP_PASSWORD,
        BANDIT_OTA_AP_ADDRESS
    );

    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi stop failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            ESP_LOGE(TAG, "failed to create AP netif");
            return ESP_FAIL;
        }
    }

    wifi_config_t ap_config = {0};
    snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "%s", BANDIT_OTA_AP_SSID);
    snprintf(
        (char *)ap_config.ap.password,
        sizeof(ap_config.ap.password),
        "%s",
        BANDIT_OTA_AP_PASSWORD
    );
    ap_config.ap.ssid_len = strlen(BANDIT_OTA_AP_SSID);
    ap_config.ap.channel = 6;
    ap_config.ap.max_connection = 2;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.pmf_cfg.capable = true;
    ap_config.ap.pmf_cfg.required = false;

    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        return ret;
    }

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    // Keep OTA buffers off this task's stack, and retain extra headroom for
    // HTTP parsing, Wi-Fi/TCP callbacks, and response handling.
    server_config.stack_size = 12288;
    server_config.max_uri_handlers = 5;
    server_config.uri_match_fn = httpd_uri_match_wildcard;

    ret = httpd_start(&s_server, &server_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t update = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = update_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t wifi_config = {
        .uri = "/wifi",
        .method = HTTP_POST,
        .handler = wifi_config_post_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t probe = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = probe_get_handler,
        .user_ctx = NULL,
    };

    ret = httpd_register_uri_handler(s_server, &root);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = httpd_register_uri_handler(s_server, &update);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = httpd_register_uri_handler(s_server, &wifi_config);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = httpd_register_uri_handler(s_server, &probe);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(
        TAG,
        "OTA update mode ready: SSID=%s address=http://%s/",
        BANDIT_OTA_AP_SSID,
        BANDIT_OTA_AP_ADDRESS
    );

    return ESP_OK;
}

void bandit_ota_confirm_running_image(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        return;
    }

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t ret = esp_ota_mark_app_valid_cancel_rollback();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "confirmed OTA image %s as valid", running->label);
        } else {
            ESP_LOGE(TAG, "failed to confirm OTA image: %s", esp_err_to_name(ret));
        }
    }
}

void bandit_ota_rollback_if_pending(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        return;
    }

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGE(TAG, "critical startup failed; rolling back OTA image");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}
