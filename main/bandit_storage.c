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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

#define BANDIT_LOG_DIR BSP_SD_MOUNT_POINT "/bandic5"
#define BANDIT_SESSION_PATH_MAX 96
#define BANDIT_SESSION_LIMIT 9999
#define BANDIT_FLUSH_SCAN_INTERVAL 3
#define BANDIT_STORAGE_RETRY_MS 5000

static const char *TAG = "bandit_storage";

struct bandit_storage_reader {
    FILE *file;
    size_t size_bytes;
    unsigned index;
};

static FILE *s_log_file;
static bandit_storage_state_t s_state = BANDIT_STORAGE_NO_CARD;
static char s_session_path[BANDIT_SESSION_PATH_MAX];
static uint32_t s_scans_since_flush;
static bool s_mounted;
static bool s_unmount_pending;
static unsigned s_reader_count;
static int64_t s_next_retry_ms;
static SemaphoreHandle_t s_mutex;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void schedule_retry_locked(void)
{
    s_next_retry_ms = now_ms() + BANDIT_STORAGE_RETRY_MS;
}

static bool parse_session_filename(const char *name, unsigned *index)
{
    if (!name || strlen(name) != strlen("session-0000.csv")) {
        return false;
    }

    if (strncmp(name, "session-", 8) != 0 ||
        strcmp(name + 12, ".csv") != 0) {
        return false;
    }

    unsigned value = 0;
    for (size_t i = 8; i < 12; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
        value = (value * 10U) + (unsigned)(name[i] - '0');
    }

    if (value == 0 || value > BANDIT_SESSION_LIMIT) {
        return false;
    }

    if (index) {
        *index = value;
    }
    return true;
}

static void unmount_now_locked(void)
{
    if (!s_mounted) {
        s_unmount_pending = false;
        return;
    }

    esp_err_t ret = bsp_sdcard_unmount();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TF unmount returned %s", esp_err_to_name(ret));
    }

    s_mounted = false;
    s_unmount_pending = false;
}

static void complete_pending_unmount_locked(void)
{
    if (s_unmount_pending && s_reader_count == 0) {
        unmount_now_locked();
    }
}

static void teardown_storage_locked(bandit_storage_state_t next_state)
{
    if (s_log_file) {
        (void)fclose(s_log_file);
        s_log_file = NULL;
    }

    // Publish the volume as unavailable before touching the FAT/VFS mount so
    // the HTTP task cannot acquire a new reader while teardown is in progress.
    s_state = next_state;
    s_session_path[0] = '\0';
    s_scans_since_flush = 0;

    if (s_mounted) {
        if (s_reader_count == 0) {
            unmount_now_locked();
        } else {
            s_unmount_pending = true;
            ESP_LOGW(
                TAG,
                "deferring TF unmount for %u active reader(s)",
                s_reader_count
            );
        }
    }
}

static void set_error_locked(const char *reason)
{
    ESP_LOGE(TAG, "%s", reason ? reason : "storage error");
    teardown_storage_locked(BANDIT_STORAGE_ERROR);
    schedule_retry_locked();
}

static esp_err_t flush_log_locked(void)
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

static esp_err_t ensure_log_directory_locked(void)
{
    if (mkdir(BANDIT_LOG_DIR, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "mkdir %s failed: %s", BANDIT_LOG_DIR, strerror(errno));
    return ESP_FAIL;
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

static esp_err_t choose_session_path_locked(void)
{
    DIR *dir = opendir(BANDIT_LOG_DIR);
    if (!dir) {
        ESP_LOGE(TAG, "opendir %s failed: %s", BANDIT_LOG_DIR, strerror(errno));
        return ESP_FAIL;
    }

    unsigned highest = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        unsigned index = 0;
        if (parse_session_filename(entry->d_name, &index) && index > highest) {
            highest = index;
        }
    }
    closedir(dir);

    if (highest >= BANDIT_SESSION_LIMIT) {
        ESP_LOGE(TAG, "session namespace exhausted");
        return ESP_ERR_NO_MEM;
    }

    return bandit_storage_session_path(
        highest + 1U,
        s_session_path,
        sizeof(s_session_path)
    );
}

static bool csv_formula_prefix_needed(const char *text)
{
    if (!text || !text[0]) {
        return false;
    }

    return text[0] == '=' ||
           text[0] == '+' ||
           text[0] == '-' ||
           text[0] == '@' ||
           text[0] == '\t' ||
           text[0] == '\r';
}

static bool write_csv_text(FILE *file, const char *text)
{
    if (fputc('"', file) == EOF) {
        return false;
    }

    const char *safe = text ? text : "";
    if (csv_formula_prefix_needed(safe) && fputc('\'', file) == EOF) {
        return false;
    }

    for (const char *p = safe; *p; p++) {
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

static esp_err_t start_session_locked(void)
{
    complete_pending_unmount_locked();

    if (s_reader_count != 0 || s_unmount_pending) {
        schedule_retry_locked();
        return ESP_ERR_INVALID_STATE;
    }

    if (s_log_file || s_mounted) {
        teardown_storage_locked(BANDIT_STORAGE_NO_CARD);
        complete_pending_unmount_locked();
        if (s_mounted) {
            schedule_retry_locked();
            return ESP_ERR_INVALID_STATE;
        }
    }

    esp_err_t ret = bsp_sdcard_mount();
    if (ret != ESP_OK) {
        s_state = BANDIT_STORAGE_NO_CARD;
        s_session_path[0] = '\0';
        schedule_retry_locked();
        return ret;
    }
    s_mounted = true;

    ret = ensure_log_directory_locked();
    if (ret != ESP_OK) {
        teardown_storage_locked(BANDIT_STORAGE_ERROR);
        schedule_retry_locked();
        return ret;
    }

    ret = choose_session_path_locked();
    if (ret != ESP_OK) {
        teardown_storage_locked(BANDIT_STORAGE_ERROR);
        schedule_retry_locked();
        return ret;
    }

    s_log_file = fopen(s_session_path, "wb");
    if (!s_log_file) {
        ESP_LOGE(TAG, "open %s failed: %s", s_session_path, strerror(errno));
        teardown_storage_locked(BANDIT_STORAGE_ERROR);
        schedule_retry_locked();
        return ESP_FAIL;
    }

    if (fputs(
            "uptime_ms,scan,bssid,ssid,rssi,channel,band,auth,hidden\n",
            s_log_file
        ) == EOF ||
        flush_log_locked() != ESP_OK) {
        char failed_path[BANDIT_SESSION_PATH_MAX];
        snprintf(failed_path, sizeof(failed_path), "%s", s_session_path);
        (void)fclose(s_log_file);
        s_log_file = NULL;
        (void)unlink(failed_path);
        set_error_locked("failed to initialize session log");
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
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    teardown_storage_locked(BANDIT_STORAGE_NO_CARD);
    s_next_retry_ms = 0;
    esp_err_t ret = start_session_locked();
    xSemaphoreGive(s_mutex);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TF card unavailable: %s", esp_err_to_name(ret));
    }

    return ret;
}

void bandit_storage_service(void)
{
    if (!s_mutex) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    complete_pending_unmount_locked();

    if (s_state == BANDIT_STORAGE_READY ||
        s_reader_count != 0 ||
        s_unmount_pending) {
        xSemaphoreGive(s_mutex);
        return;
    }

    const int64_t current_ms = now_ms();
    if (s_next_retry_ms != 0 && current_ms < s_next_retry_ms) {
        xSemaphoreGive(s_mutex);
        return;
    }

    ESP_LOGI(TAG, "retrying TF card mount");

    esp_err_t ret = start_session_locked();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "TF card recovered");
    } else {
        if (s_state != BANDIT_STORAGE_ERROR) {
            s_state = BANDIT_STORAGE_NO_CARD;
        }
        schedule_retry_locked();
        ESP_LOGD(TAG, "TF retry unavailable: %s", esp_err_to_name(ret));
    }

    xSemaphoreGive(s_mutex);
}

void bandit_storage_log_record(const bandit_scan_record_t *record, void *ctx)
{
    (void)ctx;

    if (!record || !s_mutex) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_state != BANDIT_STORAGE_READY || !s_log_file) {
        xSemaphoreGive(s_mutex);
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
        set_error_locked("TF write failed");
    }

    xSemaphoreGive(s_mutex);
}

void bandit_storage_finish_scan(uint32_t generation)
{
    (void)generation;

    if (!s_mutex) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_state != BANDIT_STORAGE_READY || !s_log_file) {
        xSemaphoreGive(s_mutex);
        return;
    }

    s_scans_since_flush++;
    if (s_scans_since_flush < BANDIT_FLUSH_SCAN_INTERVAL) {
        xSemaphoreGive(s_mutex);
        return;
    }

    if (flush_log_locked() != ESP_OK) {
        set_error_locked("TF flush failed");
        xSemaphoreGive(s_mutex);
        return;
    }

    s_scans_since_flush = 0;
    xSemaphoreGive(s_mutex);
}

bandit_storage_state_t bandit_storage_get_state(void)
{
    if (!s_mutex) {
        return s_state;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bandit_storage_state_t state = s_state;
    xSemaphoreGive(s_mutex);
    return state;
}

esp_err_t bandit_storage_get_session_path(char *path, size_t path_size)
{
    if (!path || path_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_mutex) {
        path[0] = '\0';
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int written = snprintf(path, path_size, "%s", s_session_path);
    xSemaphoreGive(s_mutex);

    if (written < 0 || written >= (int)path_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return path[0] ? ESP_OK : ESP_ERR_INVALID_STATE;
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

esp_err_t bandit_storage_list_sessions(
    bandit_storage_session_t *sessions,
    size_t capacity,
    size_t *count
)
{
    if (!sessions || capacity == 0 || !count || !s_mutex) {
        return ESP_ERR_INVALID_ARG;
    }

    *count = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (!s_mounted ||
        s_state != BANDIT_STORAGE_READY ||
        s_unmount_pending) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    DIR *dir = opendir(BANDIT_LOG_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "opendir %s failed: %s", BANDIT_LOG_DIR, strerror(errno));
        xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        unsigned index = 0;
        if (!parse_session_filename(entry->d_name, &index)) {
            continue;
        }

        bandit_storage_session_t candidate = {
            .index = index,
            .size_bytes = 0,
            .active = false,
        };

        if (*count < capacity) {
            sessions[(*count)++] = candidate;
            continue;
        }

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

    size_t valid_count = 0;
    for (size_t i = 0; i < *count; i++) {
        char path[BANDIT_SESSION_PATH_MAX];
        if (bandit_storage_session_path(
                sessions[i].index,
                path,
                sizeof(path)
            ) != ESP_OK) {
            continue;
        }

        struct stat st;
        if (stat(path, &st) != 0 || st.st_size < 0) {
            continue;
        }

        sessions[valid_count] = sessions[i];
        sessions[valid_count].size_bytes = (size_t)st.st_size;
        sessions[valid_count].active =
            strcmp(path, s_session_path) == 0;
        valid_count++;
    }

    *count = valid_count;
    qsort(sessions, *count, sizeof(*sessions), compare_session_desc);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t bandit_storage_open_completed_session(
    unsigned index,
    bandit_storage_reader_t **reader,
    size_t *size_bytes
)
{
    if (!reader || !size_bytes || !s_mutex) {
        return ESP_ERR_INVALID_ARG;
    }

    *reader = NULL;
    *size_bytes = 0;

    bandit_storage_reader_t *candidate = calloc(1, sizeof(*candidate));
    if (!candidate) {
        return ESP_ERR_NO_MEM;
    }

    char path[BANDIT_SESSION_PATH_MAX];
    esp_err_t ret = bandit_storage_session_path(index, path, sizeof(path));
    if (ret != ESP_OK) {
        free(candidate);
        return ret;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (!s_mounted ||
        s_state != BANDIT_STORAGE_READY ||
        s_unmount_pending) {
        xSemaphoreGive(s_mutex);
        free(candidate);
        return ESP_ERR_INVALID_STATE;
    }

    if (strcmp(path, s_session_path) == 0) {
        xSemaphoreGive(s_mutex);
        free(candidate);
        return ESP_ERR_INVALID_STATE;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        xSemaphoreGive(s_mutex);
        free(candidate);
        return ESP_ERR_NOT_FOUND;
    }

    struct stat st;
    if (fstat(fileno(file), &st) != 0 || st.st_size < 0) {
        fclose(file);
        xSemaphoreGive(s_mutex);
        free(candidate);
        return ESP_FAIL;
    }

    candidate->file = file;
    candidate->size_bytes = (size_t)st.st_size;
    candidate->index = index;
    s_reader_count++;

    xSemaphoreGive(s_mutex);

    *reader = candidate;
    *size_bytes = candidate->size_bytes;
    return ESP_OK;
}

size_t bandit_storage_read_completed_session(
    bandit_storage_reader_t *reader,
    void *buffer,
    size_t size,
    bool *eof
)
{
    if (eof) {
        *eof = false;
    }

    if (!reader || !reader->file || !buffer || size == 0) {
        return 0;
    }

    size_t count = fread(buffer, 1, size, reader->file);
    if (eof) {
        *eof = feof(reader->file) != 0;
    }
    return count;
}

void bandit_storage_close_completed_session(bandit_storage_reader_t *reader)
{
    if (!reader) {
        return;
    }

    if (reader->file) {
        (void)fclose(reader->file);
        reader->file = NULL;
    }

    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);

        if (s_reader_count > 0) {
            s_reader_count--;
        }
        complete_pending_unmount_locked();

        xSemaphoreGive(s_mutex);
    }

    free(reader);
}

void bandit_storage_deinit(void)
{
    if (!s_mutex) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_log_file) {
        (void)flush_log_locked();
    }

    teardown_storage_locked(BANDIT_STORAGE_NO_CARD);
    s_next_retry_ms = 0;

    xSemaphoreGive(s_mutex);
}
