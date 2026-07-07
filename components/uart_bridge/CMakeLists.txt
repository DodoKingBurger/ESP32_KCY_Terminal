idf_component_register(
    SRCS
        "uart_bridge.c"
        "modbus_master.c"
        "mbcrc.c"
        "parser.c"
        "telemetry.c"
        "modbus_poll.c"

    INCLUDE_DIRS "."
    
    REQUIRES 
        driver 
        esp_driver_uart 
        esp_driver_gpio      
        log   
        uart_bridge
        freertos
        web_server 

)