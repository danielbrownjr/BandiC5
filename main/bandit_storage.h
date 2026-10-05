#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "bandit_scan.h"

typedef enum {
    BANDIT_STORAGE_NO_CARD = 0,
    BANDIT_STORAGE_READY,
    BANDIT_STORAGE_ERROR,
} bandit_storage_state_t;

typedef struct {
    unsigned index;
    size_t size_bytes;
    bool active;
} bandit_storage_session_t;

esp_err_t bandit_storage_init(void);
void bandit_storage_service(void);
void bandit_storage_log_record(const bandit_scan_record_t *record, void *ctx);
void bandit_storage_finish_scan(uint32_t generation);
bandit_storage_state_t bandit_storage_get_state(void);
const char *bandit_storage_get_session_path(void);
esp_err_t bandit_storage_list_sessions(
    bandit_storage_session_t *sessions,
    size_t capacity,
    size_t *count
);
esp_err_t bandit_storage_session_path(
    unsigned index,
    char *path,
    size_t path_size
);
void bandit_storage_deinit(void);
