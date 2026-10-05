#pragma once

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

esp_err_t bandit_scan_init(void);
esp_err_t bandit_scan_once(bandit_scan_snapshot_t *snapshot);
