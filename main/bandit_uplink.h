#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "bandit_scan.h"

esp_err_t bandit_uplink_init(void);
void bandit_uplink_service(void);
void bandit_uplink_stop(void);
void bandit_uplink_begin_scan(void);
void bandit_uplink_end_scan(void);
void bandit_uplink_observe_record(const bandit_scan_record_t *record);
void bandit_uplink_publish_snapshot(const bandit_scan_snapshot_t *snapshot);

bool bandit_uplink_get_configured_ssid(char out_ssid[33]);
esp_err_t bandit_uplink_save_credentials(const char *ssid, const char *password);
esp_err_t bandit_uplink_clear_credentials(void);
