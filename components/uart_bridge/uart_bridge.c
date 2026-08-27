#include "uart_bridge.h"


#define UART_TX_PIN CONFIG_EXAMPLE_UART_TXD
#define UART_RX_PIN CONFIG_EXAMPLE_UART_RXD

#define UART_BAUD_RATE CONFIG_EXAMPLE_UART_BAUD_RATE

#define UART_BUF_SIZE 32768  

#define UART_PORT (CONFIG_EXAMPLE_UART_PORT_NUM)

static SemaphoreHandle_t uart_mutex = NULL;

/**
 * @brief Инициализация UART для обмена с Modbus-устройством
 */
void uart_bridge_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };

    /*
    UART PARAMS
    */

    uart_param_config(
        UART_PORT,
        &uart_config
    );

    /*
    UART PINS
    */

    uart_set_pin(
        UART_PORT,
        UART_TX_PIN,
        UART_RX_PIN,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE
    );

    /*
    DRIVER INSTALL
    */

    uart_driver_install(
        UART_PORT,
        UART_BUF_SIZE,
        UART_BUF_SIZE,
        20,
        NULL,
        0
    );

    /*
    CLEAN RX BUFFER
    */

    uart_flush(UART_PORT);

    uart_mutex = xSemaphoreCreateMutex();
    if (uart_mutex == NULL) {
        ESP_LOGE("UART", "Failed to create mutex");
    }
}

/**
 * Modbus RTU-приём:
 *  - ждём первый байт до timeout_ms
 *  - дальше копим данные с коротким inter-byte timeout
 */
#ifndef UART_INTERBYTE_MS
#define UART_INTERBYTE_MS 15
#endif

static uint32_t tx_wait_ms(size_t tx_len)
{
    const uint32_t baud = (uint32_t)UART_BAUD_RATE;
    uint32_t tx_ms = 50;
    if (baud > 0) {
        tx_ms = (uint32_t)((tx_len * 11ull * 1000ull) / baud) + 20;
    }
    if (tx_ms < 50) tx_ms = 50;
    if (tx_ms > 5000) tx_ms = 5000;
    return tx_ms;
}

static int transact_rx(uint8_t *rx, size_t rx_max, uint32_t timeout_ms)
{
    int n = uart_read_bytes(UART_PORT, rx, 1, pdMS_TO_TICKS(timeout_ms));
    if (n <= 0) {
        return 0;
    }
    int total = n;
    while ((size_t)total < rx_max) {
        n = uart_read_bytes(
            UART_PORT,
            rx + total,
            rx_max - (size_t)total,
            pdMS_TO_TICKS(UART_INTERBYTE_MS)
        );
        if (n <= 0) {
            break;
        }
        total += n;
    }
    return total;
}

/**
 * Путь для архива/терминала/регистров — без post-TX flush.
 * Поведение как до внедрения 0x65.
 */
int uart_bridge_transact(
    const uint8_t *tx,
    size_t tx_len,
    uint8_t *rx,
    size_t rx_max,
    uint32_t timeout_ms
)
{
    if (uart_mutex == NULL || tx == NULL || rx == NULL || tx_len == 0 || rx_max == 0) {
        return -1;
    }

    xSemaphoreTake(uart_mutex, portMAX_DELAY);

    uart_flush(UART_PORT);

    int written = uart_write_bytes(UART_PORT, tx, tx_len);
    if (written != (int)tx_len) {
        ESP_LOGW("UART", "write short: %d / %u", written, (unsigned)tx_len);
        xSemaphoreGive(uart_mutex);
        return -1;
    }

    uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(tx_wait_ms(tx_len)));

    int total = transact_rx(rx, rx_max, timeout_ms);

    xSemaphoreGive(uart_mutex);
    return total;
}

/**
 * Только FC 0x65 (длинный TX). Архив сюда не ходит.
 */
int uart_bridge_transact_long_tx(
    const uint8_t *tx,
    size_t tx_len,
    uint8_t *rx,
    size_t rx_max,
    uint32_t timeout_ms
)
{
    if (uart_mutex == NULL || tx == NULL || rx == NULL || tx_len == 0 || rx_max == 0) {
        return -1;
    }

    xSemaphoreTake(uart_mutex, portMAX_DELAY);

    uart_flush(UART_PORT);

    int written = uart_write_bytes(UART_PORT, tx, tx_len);
    if (written != (int)tx_len) {
        ESP_LOGW("UART", "long TX write short: %d / %u", written, (unsigned)tx_len);
        xSemaphoreGive(uart_mutex);
        return -1;
    }

    uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(tx_wait_ms(tx_len)));

    /*
     * Эхо half-duplex уже в FIFO. Ответ КСУ на запись порции — после обработки.
     * Сбрасываем эхо; пауза даёт время на запись во flash.
     */
    uart_flush(UART_PORT);
    vTaskDelay(pdMS_TO_TICKS(50));
    uart_flush(UART_PORT);

    int total = transact_rx(rx, rx_max, timeout_ms);

    xSemaphoreGive(uart_mutex);
    return total;
}
