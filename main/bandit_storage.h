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

typedef struct bandit_storage_reader bandit_storage_reader_t;

esp_err_t bandit_storage_init(void);
void bandit_storage_service(void);
void bandit_storage_log_record(const bandit_scan_record_t *record, void *ctx);
void bandit_storage_finish_scan(uint32_t generation);
bandit_storage_state_t bandit_storage_get_state(void);
esp_err_t bandit_storage_get_session_path(
    char *path,
    size_t path_size
);
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
esp_err_t bandit_storage_open_completed_session(
    unsigned index,
    bandit_storage_reader_t **reader,
    size_t *size_bytes
);
size_t bandit_storage_read_completed_session(
    bandit_storage_reader_t *reader,
    void *buffer,
    size_t size,
    bool *eof
);
void bandit_storage_close_completed_session(bandit_storage_reader_t *reader);
void bandit_storage_deinit(void);
