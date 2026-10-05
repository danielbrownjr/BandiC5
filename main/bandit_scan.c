#include "bandit_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#define SCAN_ACTIVE_MIN_MS 80
#define SCAN_ACTIVE_MAX_MS 220

static const char *TAG = "bandit_scan";
static uint32_t s_generation;

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS failed");
        ret = nvs_flash_init();
    }
    return ret;
}

static void ssid_to_text(const uint8_t *ssid, char out[33])
{
    size_t len = 0;

    while (len < 32 && ssid[len] != 0) {
        const uint8_t ch = ssid[len];
        out[len] = (ch >= 0x20 && ch <= 0x7e) ? (char)ch : '?';
        len++;
    }

    if (len == 0) {
        snprintf(out, 33, "<hidden>");
        return;
    }

    out[len] = 0;
}

static const char *auth_name(wifi_auth_mode_t auth)
{
    switch (auth) {
    case WIFI_AUTH_OPEN:
        return "OPEN";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA1/2";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/3";
    default:
        return "SEC";
    }
}

esp_err_t bandit_scan_init(void)
{
    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "NVS init failed");

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    if (!esp_netif_create_default_wifi_sta()) {
        ESP_LOGW(TAG, "default Wi-Fi STA netif was not created");
    }

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_cfg), TAG, "Wi-Fi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "STA mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start failed");

    // ESP32-C5 defaults to AUTO, but make 2.4 + 5 GHz intent explicit.
    ESP_RETURN_ON_ERROR(
        esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO),
        TAG,
        "dual-band AUTO mode failed"
    );

    wifi_band_mode_t band_mode = WIFI_BAND_MODE_2G_ONLY;
    ESP_RETURN_ON_ERROR(esp_wifi_get_band_mode(&band_mode), TAG, "read band mode failed");

    ESP_LOGI(TAG, "radio ready, band mode=%d", (int)band_mode);
    return ESP_OK;
}

esp_err_t bandit_scan_once(bandit_scan_snapshot_t *snapshot)
{
    if (!snapshot) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->strongest_rssi = -127;
    snprintf(snapshot->strongest_ssid, sizeof(snapshot->strongest_ssid), "none");
    snprintf(snapshot->strongest_auth, sizeof(snapshot->strongest_auth), "--");

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {
            .min = SCAN_ACTIVE_MIN_MS,
            .max = SCAN_ACTIVE_MAX_MS,
        },
    };

    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&scan_cfg, true), TAG, "scan failed");

    uint16_t ap_count = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&ap_count), TAG, "AP count failed");

    snapshot->generation = ++s_generation;
    snapshot->total = ap_count;

    if (ap_count == 0) {
        return ESP_OK;
    }

    wifi_ap_record_t *records = calloc(ap_count, sizeof(*records));
    if (!records) {
        return ESP_ERR_NO_MEM;
    }

    uint16_t record_count = ap_count;
    esp_err_t ret = esp_wifi_scan_get_ap_records(&record_count, records);
    if (ret != ESP_OK) {
        free(records);
        return ret;
    }

    for (uint16_t i = 0; i < record_count; i++) {
        const wifi_ap_record_t *record = &records[i];

        if (record->primary > 14) {
            snapshot->count_5g++;
        } else {
            snapshot->count_2g++;
        }

        if (record->ssid[0] == 0) {
            snapshot->hidden++;
        }

        if (record->authmode == WIFI_AUTH_OPEN) {
            snapshot->open++;
        }

        if (record->rssi > snapshot->strongest_rssi) {
            snapshot->strongest_rssi = record->rssi;
            snapshot->strongest_channel = record->primary;
            ssid_to_text(record->ssid, snapshot->strongest_ssid);
            snprintf(
                snapshot->strongest_auth,
                sizeof(snapshot->strongest_auth),
                "%s",
                auth_name(record->authmode)
            );
        }
    }

    free(records);
    return ESP_OK;
}
