#include "bandit_storage.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"

#define BANDIT_LOG_DIR BSP_SD_MOUNT_POINT "/bandic5"
#define BANDIT_SESSION_PATH_MAX 96
#define BANDIT_SESSION_LIMIT 9999
#define BANDIT_FLUSH_SCAN_INTERVAL 3

static const char *TAG = "bandit_storage";

static FILE *s_log_file;
static bandit_storage_state_t s_state = BANDIT_STORAGE_NO_CARD;
static char s_session_path[BANDIT_SESSION_PATH_MAX];
static uint32_t s_scans_since_flush;

static void set_error(const char *reason)
{
    ESP_LOGE(TAG, "%s", reason ? reason : "storage error");
    s_state = BANDIT_STORAGE_ERROR;

    if (s_log_file) {
        fclose(s_log_file);
        s_log_file = NULL;
    }
}

static esp_err_t flush_log(void)
{
    if (!s_log_file) {
        return ESP_ERR_INVALID_STATE;
    }

    if (fflush(s_log_file) != 0) {
        ESP_LOGE(TAG, "fflush failed: %s", strerror(errno));
        return ESP_FAIL;
    }

    int fd = fileno(s_log_file);
    if (fd >= 0 && fsync(fd) != 0) {
        ESP_LOGE(TAG, "fsync failed: %s", strerror(errno));
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t ensure_log_directory(void)
{
    if (mkdir(BANDIT_LOG_DIR, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "mkdir %s failed: %s", BANDIT_LOG_DIR, strerror(errno));
    return ESP_FAIL;
}

static esp_err_t choose_session_path(void)
{
    struct stat st;

    for (unsigned index = 1; index <= BANDIT_SESSION_LIMIT; index++) {
        int written = snprintf(
            s_session_path,
            sizeof(s_session_path),
            BANDIT_LOG_DIR "/session-%04u.csv",
            index
        );

        if (written <= 0 || written >= (int)sizeof(s_session_path)) {
            return ESP_ERR_INVALID_SIZE;
        }

        if (stat(s_session_path, &st) != 0) {
            if (errno == ENOENT) {
                return ESP_OK;
            }

            ESP_LOGE(TAG, "stat %s failed: %s", s_session_path, strerror(errno));
            return ESP_FAIL;
        }
    }

    ESP_LOGE(TAG, "session namespace exhausted");
    return ESP_ERR_NO_MEM;
}

static bool write_csv_text(FILE *file, const char *text)
{
    if (fputc('"', file) == EOF) {
        return false;
    }

    for (const char *p = text ? text : ""; *p; p++) {
        if (*p == '"') {
            if (fputc('"', file) == EOF) {
                return false;
            }
        }

        if (fputc(*p, file) == EOF) {
            return false;
        }
    }

    return fputc('"', file) != EOF;
}

esp_err_t bandit_storage_init(void)
{
    s_state = BANDIT_STORAGE_NO_CARD;
    s_session_path[0] = '\0';
    s_scans_since_flush = 0;

    esp_err_t ret = bsp_sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TF card unavailable: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ensure_log_directory();
    if (ret != ESP_OK) {
        bsp_sdcard_unmount();
        s_state = BANDIT_STORAGE_ERROR;
        return ret;
    }

    ret = choose_session_path();
    if (ret != ESP_OK) {
        bsp_sdcard_unmount();
        s_state = BANDIT_STORAGE_ERROR;
        return ret;
    }

    s_log_file = fopen(s_session_path, "wb");
    if (!s_log_file) {
        ESP_LOGE(TAG, "open %s failed: %s", s_session_path, strerror(errno));
        bsp_sdcard_unmount();
        s_state = BANDIT_STORAGE_ERROR;
        return ESP_FAIL;
    }

    if (fputs(
            "uptime_ms,scan,bssid,ssid,rssi,channel,band,auth,hidden\n",
            s_log_file
        ) == EOF ||
        flush_log() != ESP_OK) {
        set_error("failed to initialize session log");
        bsp_sdcard_unmount();
        return ESP_FAIL;
    }

    s_state = BANDIT_STORAGE_READY;
    ESP_LOGI(TAG, "logging to %s", s_session_path);
    return ESP_OK;
}

void bandit_storage_log_record(const bandit_scan_record_t *record, void *ctx)
{
    (void)ctx;

    if (!record || s_state != BANDIT_STORAGE_READY || !s_log_file) {
        return;
    }

    char bssid[18];
    snprintf(
        bssid,
        sizeof(bssid),
        "%02X:%02X:%02X:%02X:%02X:%02X",
        record->bssid[0],
        record->bssid[1],
        record->bssid[2],
        record->bssid[3],
        record->bssid[4],
        record->bssid[5]
    );

    const char *band = record->channel > 14 ? "5GHz" : "2.4GHz";

    bool ok =
        fprintf(
            s_log_file,
            "%lld,%lu,",
            (long long)record->uptime_ms,
            (unsigned long)record->generation
        ) >= 0 &&
        write_csv_text(s_log_file, bssid) &&
        fputc(',', s_log_file) != EOF &&
        write_csv_text(s_log_file, record->ssid) &&
        fprintf(
            s_log_file,
            ",%d,%u,%s,%s,%u\n",
            record->rssi,
            record->channel,
            band,
            record->auth,
            record->hidden ? 1U : 0U
        ) >= 0;

    if (!ok) {
        set_error("TF write failed");
    }
}

void bandit_storage_finish_scan(uint32_t generation)
{
    (void)generation;

    if (s_state != BANDIT_STORAGE_READY || !s_log_file) {
        return;
    }

    s_scans_since_flush++;
    if (s_scans_since_flush < BANDIT_FLUSH_SCAN_INTERVAL) {
        return;
    }

    if (flush_log() != ESP_OK) {
        set_error("TF flush failed");
        return;
    }

    s_scans_since_flush = 0;
}

bandit_storage_state_t bandit_storage_get_state(void)
{
    return s_state;
}

const char *bandit_storage_get_session_path(void)
{
    return s_session_path;
}

void bandit_storage_deinit(void)
{
    if (s_log_file) {
        (void)flush_log();
        fclose(s_log_file);
        s_log_file = NULL;
    }

    if (s_state != BANDIT_STORAGE_NO_CARD) {
        (void)bsp_sdcard_unmount();
    }

    s_state = BANDIT_STORAGE_NO_CARD;
    s_session_path[0] = '\0';
    s_scans_since_flush = 0;
}
