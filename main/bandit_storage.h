#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "bandit_scan.h"

typedef enum {
    BANDIT_STORAGE_NO_CARD = 0,
    BANDIT_STORAGE_READY,
    BANDIT_STORAGE_ERROR,
} bandit_storage_state_t;

esp_err_t bandit_storage_init(void);
void bandit_storage_log_record(const bandit_scan_record_t *record, void *ctx);
void bandit_storage_finish_scan(uint32_t generation);
bandit_storage_state_t bandit_storage_get_state(void);
const char *bandit_storage_get_session_path(void);
void bandit_storage_deinit(void);
