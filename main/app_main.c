#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "bandit_scan.h"
#include "bandit_ui.h"

#define SCAN_TASK_STACK_SIZE (8 * 1024)
#define SCAN_INTERVAL_MS 3500

static const char *TAG = "bandic5";

static void scan_task(void *arg)
{
    (void)arg;

    while (1) {
        bandit_scan_snapshot_t snapshot;

        bandit_ui_set_status("SCANNING");

        esp_err_t ret = bandit_scan_once(&snapshot);
        if (ret == ESP_OK) {
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
            bandit_ui_set_status("SCAN ERROR");
        }

        vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "BandiC5 boot");

    esp_err_t ret = bandit_ui_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(ret));
        return;
    }

    bandit_ui_set_status("RADIO INIT");

    ret = bandit_scan_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "radio init failed: %s", esp_err_to_name(ret));
        bandit_ui_set_status("RADIO ERROR");
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
    }
}
