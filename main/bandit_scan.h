#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t generation;
    uint16_t total;
    uint16_t count_2g;
    uint16_t count_5g;
    uint16_t hidden;
    uint16_t open;
    int8_t strongest_rssi;
    uint8_t strongest_channel;
    char strongest_ssid[33];
    char strongest_auth[12];
} bandit_scan_snapshot_t;

typedef struct {
    uint32_t generation;
    int64_t uptime_ms;
    uint8_t bssid[6];
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    bool hidden;
    char auth[12];
} bandit_scan_record_t;

typedef void (*bandit_scan_record_callback_t)(
    const bandit_scan_record_t *record,
    void *ctx
);

esp_err_t bandit_scan_init(void);
esp_err_t bandit_scan_once(
    bandit_scan_snapshot_t *snapshot,
    bandit_scan_record_callback_t record_callback,
    void *record_callback_ctx
);
