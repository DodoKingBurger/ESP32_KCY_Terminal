#pragma once

#include <stdint.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_log.h"

#include <stddef.h>

typedef struct
{
    uint8_t hour;
    uint8_t minute;
    uint8_t second;

} rtc_time_t;

void uart_bridge_init(void);

int uart_bridge_send(
    const uint8_t *data,
    size_t len
);

int uart_bridge_receive(
    uint8_t *data,
    size_t max_len,
    uint32_t timeout_ms
);
