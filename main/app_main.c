#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"

#include "bandit_ota.h"
#include "bandit_scan.h"
#include "bandit_storage.h"
#include "bandit_ui.h"
#include "bandit_version.h"

#define SCAN_TASK_STACK_SIZE (8 * 1024)
#define SCAN_INTERVAL_MS 3500
#define OTA_REQUEST_POLL_MS 100

static const char *TAG = "bandic5";

static bandit_ui_storage_state_t storage_ui_state(void)
{
    switch (bandit_storage_get_state()) {
    case BANDIT_STORAGE_READY:
        return BANDIT_UI_STORAGE_READY;
    case BANDIT_STORAGE_ERROR:
        return BANDIT_UI_STORAGE_ERROR;
    case BANDIT_STORAGE_NO_CARD:
    default:
        return BANDIT_UI_STORAGE_NONE;
    }
}

static void enter_ota_mode(void)
{
    ESP_LOGI(TAG, "entering phone OTA update mode");

    // Close and sync the active TF session before repurposing Wi-Fi for the updater.
    bandit_storage_deinit();

    esp_err_t ret = bandit_ota_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA mode failed to start: %s", esp_err_to_name(ret));
        bandit_ui_set_ota_progress(0, "OTA START FAILED");
        vTaskDelay(pdMS_TO_TICKS(2500));
        esp_restart();
    }

    // The HTTP server owns the updater from here. This task is no longer needed.
    vTaskDelete(NULL);
}

static void wait_for_next_scan_or_ota(void)
{
    int remaining_ms = SCAN_INTERVAL_MS;

    while (remaining_ms > 0) {
        if (bandit_ota_requested()) {
            return;
        }

        int delay_ms = remaining_ms < OTA_REQUEST_POLL_MS
            ? remaining_ms
            : OTA_REQUEST_POLL_MS;

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        remaining_ms -= delay_ms;
    }
}

static void scan_task(void *arg)
{
    (void)arg;

    while (1) {
        if (bandit_ota_requested()) {
            enter_ota_mode();
        }

        bandit_scan_snapshot_t snapshot;
        bandit_ui_set_status("SCANNING");

        esp_err_t ret = bandit_scan_once(
            &snapshot,
            bandit_storage_log_record,
            NULL
        );

        if (ret == ESP_OK) {
            bandit_storage_finish_scan(snapshot.generation);
            bandit_ui_set_storage_state(storage_ui_state());
            bandit_ui_update(&snapshot);

            ESP_LOGI(
                TAG,
                "scan=%lu aps=%u 2g=%u 5g=%u strongest=%s rssi=%d ch=%u",
                (unsigned long)snapshot.generation,
                snapshot.total,
                snapshot.count_2g,
                snapshot.count_5g,
                snapshot.strongest_ssid,
                snapshot.strongest_rssi,
                snapshot.strongest_channel
            );
        } else {
            ESP_LOGE(TAG, "scan failed: %s", esp_err_to_name(ret));
            bandit_ui_set_storage_state(storage_ui_state());
            bandit_ui_set_status("SCAN ERROR");
        }

        if (bandit_ota_requested()) {
            enter_ota_mode();
        }

        wait_for_next_scan_or_ota();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "BandiC5 %s boot", BANDIT_VERSION);

    esp_err_t ret = bandit_ui_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(ret));
        bandit_ota_rollback_if_pending();
        return;
    }

    bandit_ui_set_status("SD INIT");
    ret = bandit_storage_init();
    bandit_ui_set_storage_state(storage_ui_state());

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "TF logging enabled: %s", bandit_storage_get_session_path());
    } else {
        ESP_LOGW(
            TAG,
            "TF logging unavailable; continuing without storage: %s",
            esp_err_to_name(ret)
        );
    }

    bandit_ui_set_status("RADIO INIT");

    ret = bandit_scan_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "radio init failed: %s", esp_err_to_name(ret));
        bandit_ui_set_status("RADIO ERROR");
        bandit_ota_rollback_if_pending();
        return;
    }

    ret = bandit_ota_button_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA button init failed: %s", esp_err_to_name(ret));
        bandit_ui_set_status("OTA BTN ERROR");
        bandit_ota_rollback_if_pending();
        return;
    }

    BaseType_t task_ret = xTaskCreate(
        scan_task,
        "bandit_scan",
        SCAN_TASK_STACK_SIZE,
        NULL,
        5,
        NULL
    );

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create scan task");
        bandit_ui_set_status("TASK ERROR");
        bandit_ota_rollback_if_pending();
        return;
    }

    // A newly OTA-flashed image is accepted only after display, radio, OTA trigger,
    // and the main scan task have all initialized successfully.
    bandit_ota_confirm_running_image();
}
