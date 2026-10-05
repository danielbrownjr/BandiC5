#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define BANDIT_OTA_AP_SSID "BandiC5-Update"
#define BANDIT_OTA_AP_PASSWORD "bandic5ota"
#define BANDIT_OTA_AP_ADDRESS "192.168.4.1"

esp_err_t bandit_ota_button_init(void);
bool bandit_ota_requested(void);
esp_err_t bandit_ota_start(void);

void bandit_ota_confirm_running_image(void);
void bandit_ota_rollback_if_pending(void);
