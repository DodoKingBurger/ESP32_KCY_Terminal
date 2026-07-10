idf_component_register(
    SRCS "archive_manager.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        esp_http_server
        esp_timer
        log
        web_server
        uart_bridge
)