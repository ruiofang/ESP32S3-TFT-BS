idf_component_register(
    SRCS
        "main.c"
        "web_server.c"
        "ws2812_control.c"
        "i2s_mic.c"
        "claude_ble_mode.c"
        "claude_wifi_mode.c"
        "ota_updater.c"
        "APP/lvgl_demo.c"
        "APP/font_puhui_14_1.c"
        "Lib/cJSON/cJSON.c"
    INCLUDE_DIRS
        "."
        "APP"
        "Lib"
        "Lib/cJSON"
    PRIV_REQUIRES
        bt
        spiffs
        nvs_flash
        vfs
        esp_timer
        esp_event
        esp_wifi
        esp_netif
        esp_http_server
        esp_http_client
        esp_https_ota
        app_update
        mbedtls
        driver
        esp_driver_i2s
        esp_driver_rmt
        esp_driver_gpio
        esp_driver_uart
        esp_hw_support
        log
        BSP
)

# 自动烧录到flash
# spiffs_create_partition_image(storage ../storage FLASH_IN_PROJECT)

