#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"

#include "bandit_ota.h"
#include "bandit_scan.h"
#include "bandit_storage.h"
#include "bandit_ui.h"
#include "bandit_uplink.h"
#include "bandit_version.h"

#define SCAN_TASK_STACK_SIZE (8 * 1024)
#define SCAN_INTERVAL_MS 3500
#define OTA_REQUEST_POLL_MS 100
#define OTA_CONFIRM_HEALTH_MS 30000

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

    // Stop the normal uplink/status server before repurposing Wi-Fi for the updater AP.
    bandit_uplink_stop();

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

        bandit_uplink_service();

        int delay_ms = remaining_ms < OTA_REQUEST_POLL_MS
            ? remaining_ms
            : OTA_REQUEST_POLL_MS;

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        remaining_ms -= delay_ms;
    }
}

static void handle_scan_record(const bandit_scan_record_t *record, void *ctx)
{
    (void)ctx;
    bandit_storage_log_record(record, NULL);
    bandit_uplink_observe_record(record);
}

static void scan_task(void *arg)
{
    (void)arg;

    bool first_scan_ok = false;
    bool ota_confirmed = false;
    int64_t healthy_since_ms = 0;

    while (1) {
        if (bandit_ota_requested()) {
            enter_ota_mode();
        }

        bandit_storage_service();
        bandit_ui_set_storage_state(storage_ui_state());

        bandit_scan_snapshot_t snapshot;

        if (!bandit_uplink_try_begin_scan()) {
            bandit_ui_set_status("RADIO BUSY");
            wait_for_next_scan_or_ota();
            continue;
        }

        bandit_ui_set_status("SCANNING");

        esp_err_t ret = bandit_scan_once(
            &snapshot,
            handle_scan_record,
            NULL
        );
        bandit_uplink_end_scan();

        if (ret == ESP_OK) {
            bandit_storage_finish_scan(snapshot.generation);
            bandit_ui_set_storage_state(storage_ui_state());
            bandit_ui_update(&snapshot);
            bandit_uplink_publish_snapshot(&snapshot);

            if (!first_scan_ok) {
                first_scan_ok = true;
                healthy_since_ms = esp_timer_get_time() / 1000;
            }

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
        } else if (ret == ESP_ERR_WIFI_STATE) {
            ESP_LOGW(TAG, "scan deferred while Wi-Fi driver is busy");
            bandit_ui_set_storage_state(storage_ui_state());
            bandit_ui_set_status("SCAN DEFER");
        } else {
            ESP_LOGE(TAG, "scan failed: %s", esp_err_to_name(ret));
            bandit_ui_set_storage_state(storage_ui_state());
            bandit_ui_set_status("SCAN ERROR");
        }

        if (!ota_confirmed &&
            first_scan_ok &&
            bandit_uplink_first_attempt_resolved() &&
            (esp_timer_get_time() / 1000) - healthy_since_ms >= OTA_CONFIRM_HEALTH_MS) {
            ota_confirmed = bandit_ota_confirm_running_image();
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
        char session_path[96];
        if (bandit_storage_get_session_path(
                session_path,
                sizeof(session_path)
            ) == ESP_OK) {
            ESP_LOGI(TAG, "TF logging enabled: %s", session_path);
        }
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

    ret = bandit_uplink_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "uplink unavailable; continuing scout-only: %s", esp_err_to_name(ret));
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

    // Pending OTA images are confirmed by scan_task only after real runtime
    // behavior has been exercised for the health window.
}
