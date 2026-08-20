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

/**
 * @brief Атомарный обмен: flush + TX + wait TX + RX под одним mutex.
 *        Не даёт другим задачам перехватить ответ между send и receive.
 * @param tx           запрос
 * @param tx_len       длина запроса
 * @param rx           буфер ответа
 * @param rx_max       размер буфера
 * @param timeout_ms   таймаут приёма, миллисекунды (НЕ ticks!)
 * @return число принятых байт, или -1 при ошибке отправки
 */
int uart_bridge_transact(
    const uint8_t *tx,
    size_t tx_len,
    uint8_t *rx,
    size_t rx_max,
    uint32_t timeout_ms
);
