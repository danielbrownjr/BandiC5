#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "bandit_scan.h"

typedef enum {
    BANDIT_UI_STORAGE_NONE = 0,
    BANDIT_UI_STORAGE_READY,
    BANDIT_UI_STORAGE_ERROR,
} bandit_ui_storage_state_t;

esp_err_t bandit_ui_init(void);
void bandit_ui_set_status(const char *status);
void bandit_ui_set_storage_state(bandit_ui_storage_state_t state);
void bandit_ui_set_uplink_state(bool connected);
void bandit_ui_set_uplink_address(const char *address);
void bandit_ui_update(const bandit_scan_snapshot_t *snapshot);

void bandit_ui_show_ota_mode(
    const char *ssid,
    const char *password,
    const char *address
);
void bandit_ui_set_ota_progress(int percent, const char *status);
