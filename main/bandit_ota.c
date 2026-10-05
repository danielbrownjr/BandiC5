#include "bandit_ota.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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

#define BANDIT_OTA_BOOT_GPIO GPIO_NUM_28
#define BANDIT_OTA_HOLD_MS 2000
#define BANDIT_OTA_POLL_MS 100
#define BANDIT_OTA_BUTTON_TASK_STACK 3072
#define BANDIT_OTA_RECV_BUFFER 4096

static const char *TAG = "bandit_ota";

static volatile bool s_ota_requested;
static httpd_handle_t s_server;
static esp_netif_t *s_ap_netif;

static const char s_update_page[] =
    "<!doctype html><html><head>"
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
    "</style></head><body><div class='card'>"
    "<h1>C5 Bandit Update</h1>"
    "<p>Select a <code>bandic5.bin</code> firmware image.</p>"
    "<input id='file' type='file' accept='.bin,application/octet-stream'>"
    "<button id='go'>Upload firmware</button>"
    "<progress id='bar' max='100' value='0'></progress>"
    "<p id='status'>Waiting for firmware.</p>"
    "</div><script>"
    "const f=document.getElementById('file'),s=document.getElementById('status'),"
    "b=document.getElementById('bar'),g=document.getElementById('go');"
    "g.onclick=async()=>{"
    "if(!f.files.length){s.textContent='Choose bandic5.bin first.';return;}"
    "g.disabled=true;s.textContent='Uploading...';b.value=10;"
    "try{const r=await fetch('/update',{method:'POST',headers:{'Content-Type':'application/octet-stream'},"
    "body:f.files[0]});const t=await r.text();"
    "if(!r.ok)throw new Error(t||('HTTP '+r.status));"
    "b.value=100;s.textContent='Update accepted. C5 Bandit is rebooting...';"
    "}catch(e){b.value=0;s.textContent='Update failed: '+e.message;g.disabled=false;}};"
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
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, s_update_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t update_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0) {
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
        "OTA upload: %d bytes -> %s @ 0x%08lx",
        req->content_len,
        update_partition->label,
        (unsigned long)update_partition->address
    );

    esp_ota_handle_t ota_handle = 0;
    esp_err_t ret = esp_ota_begin(update_partition, req->content_len, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to start OTA write");
        return ret;
    }

    char buffer[BANDIT_OTA_RECV_BUFFER];
    int remaining = req->content_len;
    int received_total = 0;
    int last_percent = -1;

    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(buffer) ? remaining : (int)sizeof(buffer);
        int received = httpd_req_recv(req, buffer, to_read);

        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }

        if (received <= 0) {
            ESP_LOGE(TAG, "firmware upload interrupted");
            esp_ota_abort(ota_handle);
            bandit_ui_set_ota_progress(0, "UPLOAD FAILED");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload interrupted");
            return ESP_FAIL;
        }

        ret = esp_ota_write(ota_handle, buffer, received);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(ret));
            esp_ota_abort(ota_handle);
            bandit_ui_set_ota_progress(0, "WRITE FAILED");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash write failed");
            return ret;
        }

        received_total += received;
        remaining -= received;

        int percent = (received_total * 100) / req->content_len;
        if (percent != last_percent && (percent == 100 || percent - last_percent >= 5)) {
            bandit_ui_set_ota_progress(percent, "UPLOADING");
            last_percent = percent;
        }
    }

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
    httpd_resp_sendstr(req, "Update accepted. Rebooting C5 Bandit.");

    ESP_LOGI(TAG, "OTA complete; rebooting into %s", update_partition->label);
    vTaskDelay(pdMS_TO_TICKS(1000));
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
    server_config.stack_size = 6144;
    server_config.max_uri_handlers = 4;

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

    ret = httpd_register_uri_handler(s_server, &root);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = httpd_register_uri_handler(s_server, &update);
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
