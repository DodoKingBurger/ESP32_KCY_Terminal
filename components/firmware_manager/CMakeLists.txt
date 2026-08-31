idf_component_register(
    SRCS
        "firmware_manager.c"
        "esp_ota_update.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        esp_http_server
        log
        web_server
        uart_bridge
        app_update
        esp_app_format
)
