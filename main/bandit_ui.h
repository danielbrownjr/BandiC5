#pragma once

#include "esp_err.h"

#include "bandit_scan.h"

esp_err_t bandit_ui_init(void);
void bandit_ui_set_status(const char *status);
void bandit_ui_update(const bandit_scan_snapshot_t *snapshot);
