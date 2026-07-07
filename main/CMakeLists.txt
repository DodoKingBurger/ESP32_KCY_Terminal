idf_component_register(
    SRCS "main.c"       
    REQUIRES 
        esp_driver_uart 
        esp_driver_gpio         
        wifi_manager
        web_server
        dns_server
        uart_bridge
        esp_wifi
        nvs_flash
    INCLUDE_DIRS ".")
