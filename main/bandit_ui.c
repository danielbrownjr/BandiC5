#include "bandit_ui.h"
#include "bandit_version.h"

#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "lvgl.h"

static const char *TAG = "bandit_ui";

static lv_obj_t *s_band24_value;
static lv_obj_t *s_band5_value;
static lv_obj_t *s_band24_bar;
static lv_obj_t *s_band5_bar;
static lv_obj_t *s_strongest_value;
static lv_obj_t *s_summary_value;
static lv_obj_t *s_status;
static lv_obj_t *s_storage_status;
static lv_obj_t *s_uplink_status;
static lv_obj_t *s_subtitle;
static lv_obj_t *s_ota_progress;
static lv_obj_t *s_ota_status;

#if LVGL_VERSION_MAJOR >= 9
static lv_obj_t *active_screen(void)
{
    return lv_screen_active();
}
#else
static lv_obj_t *active_screen(void)
{
    return lv_scr_act();
}
#endif

static void make_band_card(
    lv_obj_t *parent,
    int y,
    const char *name,
    lv_color_t accent,
    lv_obj_t **value_out,
    lv_obj_t **bar_out
)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 154, 52);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x151c25), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 7, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x263241), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name_label = lv_label_create(card);
    lv_label_set_text(name_label, name);
    lv_obj_set_style_text_color(name_label, accent, 0);
    lv_obj_align(name_label, LV_ALIGN_TOP_LEFT, 8, 5);

    lv_obj_t *value = lv_label_create(card);
    lv_label_set_text(value, "0 AP");
    lv_obj_set_style_text_color(value, lv_color_hex(0xf8fafc), 0);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_20, 0);
    lv_obj_align(value, LV_ALIGN_TOP_RIGHT, -8, 1);

    lv_obj_t *bar = lv_bar_create(card);
    lv_obj_set_size(bar, 138, 7);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -7);
    lv_bar_set_range(bar, 0, 50);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x283442), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, accent, LV_PART_INDICATOR);

    *value_out = value;
    *bar_out = bar;
}

static void create_ui(void)
{
    lv_obj_t *screen = active_screen();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x090d12), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "C5 BANDIT");
    lv_obj_set_style_text_color(title, lv_color_hex(0xf8fafc), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_uplink_status = lv_label_create(screen);
    lv_label_set_text(s_uplink_status, "--");
    lv_obj_set_style_text_color(s_uplink_status, lv_color_hex(0x475569), 0);
    lv_obj_align(s_uplink_status, LV_ALIGN_TOP_LEFT, 6, 10);

    s_storage_status = lv_label_create(screen);
    lv_label_set_text(s_storage_status, "--");
    lv_obj_set_style_text_color(s_storage_status, lv_color_hex(0x475569), 0);
    lv_obj_align(s_storage_status, LV_ALIGN_TOP_RIGHT, -6, 10);

    s_subtitle = lv_label_create(screen);
    lv_label_set_text(s_subtitle, BANDIT_VERSION);
    lv_obj_set_style_text_color(s_subtitle, lv_color_hex(0x7f8ea3), 0);
    lv_obj_align(s_subtitle, LV_ALIGN_TOP_MID, 0, 34);

    make_band_card(
        screen,
        58,
        "2.4 GHz",
        lv_color_hex(0x34d399),
        &s_band24_value,
        &s_band24_bar
    );

    make_band_card(
        screen,
        116,
        "5 GHz",
        lv_color_hex(0x60a5fa),
        &s_band5_value,
        &s_band5_bar
    );

    lv_obj_t *strongest = lv_obj_create(screen);
    lv_obj_remove_style_all(strongest);
    lv_obj_set_size(strongest, 154, 75);
    lv_obj_align(strongest, LV_ALIGN_TOP_MID, 0, 174);
    lv_obj_set_style_bg_color(strongest, lv_color_hex(0x111821), 0);
    lv_obj_set_style_bg_opa(strongest, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(strongest, 7, 0);
    lv_obj_set_style_border_width(strongest, 1, 0);
    lv_obj_set_style_border_color(strongest, lv_color_hex(0x263241), 0);
    lv_obj_clear_flag(strongest, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *strongest_title = lv_label_create(strongest);
    lv_label_set_text(strongest_title, "STRONGEST");
    lv_obj_set_style_text_color(strongest_title, lv_color_hex(0xf59e0b), 0);
    lv_obj_align(strongest_title, LV_ALIGN_TOP_LEFT, 8, 5);

    s_strongest_value = lv_label_create(strongest);
    lv_label_set_text(s_strongest_value, "none\n-- dBm  CH --");
    lv_obj_set_width(s_strongest_value, 138);
    lv_obj_set_style_text_color(s_strongest_value, lv_color_hex(0xe2e8f0), 0);
    lv_label_set_long_mode(s_strongest_value, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_strongest_value, LV_ALIGN_TOP_LEFT, 8, 25);

    // Keep the two-line summary above a dedicated footer band for SCANNING/READY.
    s_summary_value = lv_label_create(screen);
    lv_label_set_text(s_summary_value, "TOTAL 0   OPEN 0\nHIDDEN 0   SCAN #0");
    lv_obj_set_size(s_summary_value, 154, 34);
    lv_obj_set_style_text_color(s_summary_value, lv_color_hex(0xa8b3c2), 0);
    lv_obj_set_style_text_align(s_summary_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_summary_value, LV_ALIGN_TOP_MID, 0, 252);

    s_status = lv_label_create(screen);
    lv_label_set_text(s_status, "BOOTING");
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x34d399), 0);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -6);
}

esp_err_t bandit_ui_init(void)
{
    lv_display_t *display = bsp_display_start();
    if (!display) {
        return ESP_FAIL;
    }

    // Keep the native 172 x 320 portrait orientation for the instrument-style UI.
    ESP_RETURN_ON_ERROR(
        bsp_display_brightness_set(85),
        TAG,
        "backlight failed"
    );

    if (!bsp_display_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }

    create_ui();
    bsp_display_unlock();
    return ESP_OK;
}

void bandit_ui_set_status(const char *status)
{
    if (!status || !s_status || !bsp_display_lock(0)) {
        return;
    }

    lv_label_set_text(s_status, status);
    bsp_display_unlock();
}

void bandit_ui_set_storage_state(bandit_ui_storage_state_t state)
{
    if (!s_storage_status || !bsp_display_lock(0)) {
        return;
    }

    switch (state) {
    case BANDIT_UI_STORAGE_READY:
        lv_label_set_text(s_storage_status, "SD");
        lv_obj_set_style_text_color(s_storage_status, lv_color_hex(0x34d399), 0);
        break;
    case BANDIT_UI_STORAGE_ERROR:
        lv_label_set_text(s_storage_status, "!!");
        lv_obj_set_style_text_color(s_storage_status, lv_color_hex(0xf87171), 0);
        break;
    case BANDIT_UI_STORAGE_NONE:
    default:
        lv_label_set_text(s_storage_status, "--");
        lv_obj_set_style_text_color(s_storage_status, lv_color_hex(0x475569), 0);
        break;
    }

    bsp_display_unlock();
}

void bandit_ui_set_uplink_state(bool connected)
{
    if (!s_uplink_status || !bsp_display_lock(0)) {
        return;
    }

    lv_label_set_text(s_uplink_status, connected ? "NET" : "--");
    lv_obj_set_style_text_color(
        s_uplink_status,
        connected ? lv_color_hex(0x34d399) : lv_color_hex(0x475569),
        0
    );

    bsp_display_unlock();
}

void bandit_ui_set_uplink_address(const char *address)
{
    if (!s_subtitle || !bsp_display_lock(0)) {
        return;
    }

    lv_label_set_text(s_subtitle, (address && address[0]) ? address : BANDIT_VERSION);
    bsp_display_unlock();
}

void bandit_ui_update(const bandit_scan_snapshot_t *snapshot)
{
    if (!snapshot || !bsp_display_lock(0)) {
        return;
    }

    char text[96];

    snprintf(text, sizeof(text), "%u AP", snapshot->count_2g);
    lv_label_set_text(s_band24_value, text);
    lv_bar_set_value(
        s_band24_bar,
        snapshot->count_2g > 50 ? 50 : snapshot->count_2g,
        LV_ANIM_ON
    );

    snprintf(text, sizeof(text), "%u AP", snapshot->count_5g);
    lv_label_set_text(s_band5_value, text);
    lv_bar_set_value(
        s_band5_bar,
        snapshot->count_5g > 50 ? 50 : snapshot->count_5g,
        LV_ANIM_ON
    );

    snprintf(
        text,
        sizeof(text),
        "%s\n%d dBm  CH %u  %s",
        snapshot->strongest_ssid,
        snapshot->strongest_rssi,
        snapshot->strongest_channel,
        snapshot->strongest_auth
    );
    lv_label_set_text(s_strongest_value, text);

    snprintf(
        text,
        sizeof(text),
        "TOTAL %u   OPEN %u\nHIDDEN %u   SCAN #%lu",
        snapshot->total,
        snapshot->open,
        snapshot->hidden,
        (unsigned long)snapshot->generation
    );
    lv_label_set_text(s_summary_value, text);
    lv_label_set_text(s_status, "READY");

    bsp_display_unlock();
}


void bandit_ui_show_ota_mode(
    const char *ssid,
    const char *password,
    const char *address
)
{
    if (!bsp_display_lock(0)) {
        return;
    }

    lv_obj_t *screen = active_screen();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x090d12), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    // Normal dashboard objects no longer exist after entering update mode.
    s_band24_value = NULL;
    s_band5_value = NULL;
    s_band24_bar = NULL;
    s_band5_bar = NULL;
    s_strongest_value = NULL;
    s_summary_value = NULL;
    s_status = NULL;
    s_storage_status = NULL;
    s_uplink_status = NULL;

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "UPDATE MODE");
    lv_obj_set_style_text_color(title, lv_color_hex(0xf8fafc), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text(hint, "Connect phone to:");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x94a3b8), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 43);

    lv_obj_t *ssid_label = lv_label_create(screen);
    lv_label_set_text(ssid_label, ssid ? ssid : "BandiC5-Update");
    lv_obj_set_width(ssid_label, 160);
    lv_obj_set_style_text_align(ssid_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(ssid_label, lv_color_hex(0x34d399), 0);
    lv_obj_align(ssid_label, LV_ALIGN_TOP_MID, 0, 64);

    lv_obj_t *pass_title = lv_label_create(screen);
    lv_label_set_text(pass_title, "Password");
    lv_obj_set_style_text_color(pass_title, lv_color_hex(0x64748b), 0);
    lv_obj_align(pass_title, LV_ALIGN_TOP_MID, 0, 92);

    lv_obj_t *pass = lv_label_create(screen);
    lv_label_set_text(pass, password ? password : "");
    lv_obj_set_style_text_color(pass, lv_color_hex(0xe2e8f0), 0);
    lv_obj_align(pass, LV_ALIGN_TOP_MID, 0, 111);

    lv_obj_t *open_title = lv_label_create(screen);
    lv_label_set_text(open_title, "Then open Safari:");
    lv_obj_set_style_text_color(open_title, lv_color_hex(0x94a3b8), 0);
    lv_obj_align(open_title, LV_ALIGN_TOP_MID, 0, 143);

    char url[64];
    snprintf(url, sizeof(url), "http://%s", address ? address : "192.168.4.1");

    lv_obj_t *url_label = lv_label_create(screen);
    lv_label_set_text(url_label, url);
    lv_obj_set_width(url_label, 164);
    lv_obj_set_style_text_align(url_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(url_label, lv_color_hex(0x60a5fa), 0);
    lv_obj_align(url_label, LV_ALIGN_TOP_MID, 0, 164);

    lv_obj_t *file_hint = lv_label_create(screen);
    lv_label_set_text(file_hint, "Upload bandic5.bin");
    lv_obj_set_style_text_color(file_hint, lv_color_hex(0xcbd5e1), 0);
    lv_obj_align(file_hint, LV_ALIGN_TOP_MID, 0, 197);

    s_ota_progress = lv_bar_create(screen);
    lv_obj_set_size(s_ota_progress, 150, 10);
    lv_obj_align(s_ota_progress, LV_ALIGN_TOP_MID, 0, 226);
    lv_bar_set_range(s_ota_progress, 0, 100);
    lv_bar_set_value(s_ota_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_ota_progress, lv_color_hex(0x1e293b), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ota_progress, lv_color_hex(0x34d399), LV_PART_INDICATOR);

    s_ota_status = lv_label_create(screen);
    lv_label_set_text(s_ota_status, "WAITING FOR PHONE");
    lv_obj_set_width(s_ota_status, 160);
    lv_obj_set_style_text_align(s_ota_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_ota_status, lv_color_hex(0xf59e0b), 0);
    lv_obj_align(s_ota_status, LV_ALIGN_TOP_MID, 0, 248);

    lv_obj_t *warning = lv_label_create(screen);
    lv_label_set_text(warning, "Do not power off while uploading");
    lv_obj_set_width(warning, 164);
    lv_obj_set_style_text_align(warning, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(warning, lv_color_hex(0x64748b), 0);
    lv_obj_align(warning, LV_ALIGN_BOTTOM_MID, 0, -10);

    bsp_display_unlock();
}

void bandit_ui_set_ota_progress(int percent, const char *status)
{
    if (!s_ota_progress || !s_ota_status || !bsp_display_lock(0)) {
        return;
    }

    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }

    lv_bar_set_value(s_ota_progress, percent, LV_ANIM_ON);
    lv_label_set_text(s_ota_status, status ? status : "");

    if (percent == 100) {
        lv_obj_set_style_text_color(s_ota_status, lv_color_hex(0x34d399), 0);
    } else if (percent == 0 && status && strstr(status, "FAILED")) {
        lv_obj_set_style_text_color(s_ota_status, lv_color_hex(0xf87171), 0);
    } else {
        lv_obj_set_style_text_color(s_ota_status, lv_color_hex(0xf59e0b), 0);
    }

    bsp_display_unlock();
}
