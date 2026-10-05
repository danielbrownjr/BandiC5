idf_component_register(
    SRCS
        "app_main.c"
        "bandit_scan.c"
        "bandit_ui.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp32_c5_lcd_1_47
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        lvgl
)
