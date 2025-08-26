idf_component_register(
    SRC_DIRS 
        "." 
        "APP"
        "Lib"
    INCLUDE_DIRS 
        "."
        "APP"
        "Lib" )

# 自动烧录到flash
# spiffs_create_partition_image(storage ../storage FLASH_IN_PROJECT)

