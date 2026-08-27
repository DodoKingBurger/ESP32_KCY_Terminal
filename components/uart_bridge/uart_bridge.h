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

/**
 * @brief Обычный Modbus RTU обмен (0x03/0x04/0x06/0x64 — архив, терминал).
 *        Без post-TX flush: ответ может прийти сразу после последнего байта.
 */
int uart_bridge_transact(
    const uint8_t *tx,
    size_t tx_len,
    uint8_t *rx,
    size_t rx_max,
    uint32_t timeout_ms
);

/**
 * @brief Обмен для длинного TX (FC 0x65 прошивка).
 *        После TX сбрасывает возможное эхо, ждёт ответ отдельно.
 *        НЕ использовать для архива/терминала.
 */
int uart_bridge_transact_long_tx(
    const uint8_t *tx,
    size_t tx_len,
    uint8_t *rx,
    size_t rx_max,
    uint32_t timeout_ms
);
