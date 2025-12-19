idf_component_register(
    SRCS 
        "main.c"
        "web_server.c"
        "ws2812_control.c"
        "i2s_mic.c"
        "APP/lvgl_demo.c"
        "Lib/cJSON/cJSON.c"
    INCLUDE_DIRS 
        "."
        "APP"
        "Lib"
        "Lib/cJSON"
)

# 自动烧录到flash
# spiffs_create_partition_image(storage ../storage FLASH_IN_PROJECT)

