idf_component_register(
    SRCS "firmware_manager.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        esp_http_server
        log
        web_server
        uart_bridge
)
