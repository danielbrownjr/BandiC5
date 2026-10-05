#include "bandit_storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"

#define BANDIT_LOG_DIR BSP_SD_MOUNT_POINT "/bandic5"
#define BANDIT_SESSION_PATH_MAX 96
#define BANDIT_SESSION_LIMIT 9999
#define BANDIT_FLUSH_SCAN_INTERVAL 3
#define BANDIT_STORAGE_RETRY_MS 5000

static const char *TAG = "bandit_storage";

static FILE *s_log_file;
static bandit_storage_state_t s_state = BANDIT_STORAGE_NO_CARD;
static char s_session_path[BANDIT_SESSION_PATH_MAX];
static uint32_t s_scans_since_flush;
static bool s_mounted;
static int64_t s_next_retry_ms;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void schedule_retry(void)
{
    s_next_retry_ms = now_ms() + BANDIT_STORAGE_RETRY_MS;
}

static void teardown_storage(bandit_storage_state_t next_state)
{
    if (s_log_file) {
        (void)fclose(s_log_file);
        s_log_file = NULL;
    }

    if (s_mounted) {
        esp_err_t ret = bsp_sdcard_unmount();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "TF unmount returned %s", esp_err_to_name(ret));
        }
        s_mounted = false;
    }

    s_state = next_state;
    s_session_path[0] = '\0';
    s_scans_since_flush = 0;
}

static void set_error(const char *reason)
{
    ESP_LOGE(TAG, "%s", reason ? reason : "storage error");
    teardown_storage(BANDIT_STORAGE_ERROR);
    schedule_retry();
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

static esp_err_t start_session(void)
{
    if (s_log_file || s_mounted) {
        teardown_storage(BANDIT_STORAGE_NO_CARD);
    }

    esp_err_t ret = bsp_sdcard_mount();
    if (ret != ESP_OK) {
        s_state = BANDIT_STORAGE_NO_CARD;
        s_session_path[0] = '\0';
        schedule_retry();
        return ret;
    }
    s_mounted = true;

    ret = ensure_log_directory();
    if (ret != ESP_OK) {
        teardown_storage(BANDIT_STORAGE_ERROR);
        schedule_retry();
        return ret;
    }

    ret = choose_session_path();
    if (ret != ESP_OK) {
        teardown_storage(BANDIT_STORAGE_ERROR);
        schedule_retry();
        return ret;
    }

    s_log_file = fopen(s_session_path, "wb");
    if (!s_log_file) {
        ESP_LOGE(TAG, "open %s failed: %s", s_session_path, strerror(errno));
        teardown_storage(BANDIT_STORAGE_ERROR);
        schedule_retry();
        return ESP_FAIL;
    }

    if (fputs(
            "uptime_ms,scan,bssid,ssid,rssi,channel,band,auth,hidden\n",
            s_log_file
        ) == EOF ||
        flush_log() != ESP_OK) {
        set_error("failed to initialize session log");
        return ESP_FAIL;
    }

    s_scans_since_flush = 0;
    s_state = BANDIT_STORAGE_READY;
    s_next_retry_ms = 0;

    ESP_LOGI(TAG, "logging to %s", s_session_path);
    return ESP_OK;
}

esp_err_t bandit_storage_init(void)
{
    teardown_storage(BANDIT_STORAGE_NO_CARD);
    s_next_retry_ms = 0;

    esp_err_t ret = start_session();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TF card unavailable: %s", esp_err_to_name(ret));
    }

    return ret;
}

void bandit_storage_service(void)
{
    if (s_state == BANDIT_STORAGE_READY) {
        return;
    }

    const int64_t current_ms = now_ms();
    if (s_next_retry_ms != 0 && current_ms < s_next_retry_ms) {
        return;
    }

    ESP_LOGI(TAG, "retrying TF card mount");

    esp_err_t ret = start_session();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "TF card recovered");
    } else {
        // A mount failure means no usable card is present. Filesystem/open failures
        // leave the ERROR state set by start_session() so the UI can distinguish them.
        if (s_state != BANDIT_STORAGE_ERROR) {
            s_state = BANDIT_STORAGE_NO_CARD;
        }
        schedule_retry();
        ESP_LOGD(TAG, "TF retry unavailable: %s", esp_err_to_name(ret));
    }
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


static int compare_session_desc(const void *left, const void *right)
{
    const bandit_storage_session_t *a = left;
    const bandit_storage_session_t *b = right;

    if (a->index < b->index) {
        return 1;
    }
    if (a->index > b->index) {
        return -1;
    }
    return 0;
}

esp_err_t bandit_storage_session_path(
    unsigned index,
    char *path,
    size_t path_size
)
{
    if (!path || path_size == 0 || index == 0 || index > BANDIT_SESSION_LIMIT) {
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(
        path,
        path_size,
        BANDIT_LOG_DIR "/session-%04u.csv",
        index
    );

    if (written <= 0 || written >= (int)path_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t bandit_storage_list_sessions(
    bandit_storage_session_t *sessions,
    size_t capacity,
    size_t *count
)
{
    if (!sessions || capacity == 0 || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    *count = 0;

    if (!s_mounted || s_state != BANDIT_STORAGE_READY) {
        return ESP_ERR_INVALID_STATE;
    }

    DIR *dir = opendir(BANDIT_LOG_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "opendir %s failed: %s", BANDIT_LOG_DIR, strerror(errno));
        return ESP_FAIL;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        unsigned index = 0;
        int consumed = 0;

        if (sscanf(entry->d_name, "session-%4u.csv%n", &index, &consumed) != 1 ||
            entry->d_name[consumed] != '\0' ||
            index == 0 ||
            index > BANDIT_SESSION_LIMIT) {
            continue;
        }

        char path[BANDIT_SESSION_PATH_MAX];
        if (bandit_storage_session_path(index, path, sizeof(path)) != ESP_OK) {
            continue;
        }

        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }

        bandit_storage_session_t candidate = {
            .index = index,
            .size_bytes = (size_t)st.st_size,
            .active = strcmp(path, s_session_path) == 0,
        };

        if (*count < capacity) {
            sessions[(*count)++] = candidate;
            continue;
        }

        // Capacity is intentionally bounded for the web UI. Retain the newest
        // session numbers if the card contains more files than fit in one page.
        size_t oldest = 0;
        for (size_t i = 1; i < capacity; i++) {
            if (sessions[i].index < sessions[oldest].index) {
                oldest = i;
            }
        }

        if (candidate.index > sessions[oldest].index) {
            sessions[oldest] = candidate;
        }
    }

    closedir(dir);
    qsort(sessions, *count, sizeof(*sessions), compare_session_desc);
    return ESP_OK;
}

void bandit_storage_deinit(void)
{
    if (s_log_file) {
        (void)flush_log();
    }

    teardown_storage(BANDIT_STORAGE_NO_CARD);
    s_next_retry_ms = 0;
}
