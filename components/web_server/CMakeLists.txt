idf_component_register(
    SRCS "web_server.c" 
    INCLUDE_DIRS "."
    EMBED_FILES
        "html/index.html"
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
)