idf_component_register(
    SRCS "web_server.c"
    INCLUDE_DIRS "."
    EMBED_FILES
        "html/index.html"
        "html/style.css"
        "html/script.js"
        "html/node_modules/xterm/lib/xterm.js"
        "html/node_modules/xterm/css/xterm.css"
    REQUIRES
        esp_timer
        esp_http_server
        esp_wifi
        esp_netif
        esp_event
        log
        uart_bridge
        archive_manager
        firmware_manager
        esp_app_format
)

# Header с версией лежит в корне build/
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_BINARY_DIR}")
# Пересобрать web_server.c если версия изменилась
set_source_files_properties(web_server.c PROPERTIES
    OBJECT_DEPENDS "${CMAKE_BINARY_DIR}/esp_fw_version.h"
)
