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
 * @brief Отправка данных через UART
 * @param data указатель на данные
 * @param len  длина в байтах
 * @return количество отправленных байт или -1 при ошибке
 */
int uart_bridge_send(
    const uint8_t *data,
    size_t len
)
{
    xSemaphoreTake(uart_mutex, portMAX_DELAY);
    uart_flush(UART_PORT);
    
    int ret =
        uart_write_bytes(
            UART_PORT,
            data,
            len
        );

    /*
    Ждем завершения передачи
    */

    uart_wait_tx_done(
        UART_PORT,
        pdMS_TO_TICKS(300)
    );
    xSemaphoreGive(uart_mutex);
    return ret;
}

/**
 * @brief Приём данных из UART с таймаутом
 * @param data      буфер для приёма
 * @param max_len   максимальное количество байт
 * @param timeout_ms таймаут в миллисекундах
 * @return количество принятых байт или -1 при ошибке/таймауте
 */
int uart_bridge_receive(
    uint8_t *data,
    size_t max_len,
    uint32_t timeout_ms
)
{
    xSemaphoreTake(uart_mutex, portMAX_DELAY);
    int ret = uart_read_bytes(
        UART_PORT, 
        data, 
        max_len, 
        pdMS_TO_TICKS(timeout_ms));
    xSemaphoreGive(uart_mutex);
    return ret;
}

/**
 * Modbus RTU-приём:
 *  - ждём первый байт до timeout_ms
 *  - дальше копим данные с коротким inter-byte timeout
 * Иначе uart_read_bytes(rx_max=4096) ждёт полный timeout на КАЖДОМ кадре.
 */
#ifndef UART_INTERBYTE_MS
#define UART_INTERBYTE_MS 15
#endif

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
        xSemaphoreGive(uart_mutex);
        return -1;
    }

    uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(50));

    /* 1) Первый байт — до timeout_ms */
    int n = uart_read_bytes(UART_PORT, rx, 1, pdMS_TO_TICKS(timeout_ms));
    if (n <= 0) {
        xSemaphoreGive(uart_mutex);
        return 0;
    }
    int total = n;

    /* 2) Остаток кадра — короткие паузы между байтами */
    while ((size_t)total < rx_max) {
        n = uart_read_bytes(
            UART_PORT,
            rx + total,
            rx_max - (size_t)total,
            pdMS_TO_TICKS(UART_INTERBYTE_MS)
        );
        if (n <= 0) {
            break; /* тишина = конец кадра */
        }
        total += n;
    }

    xSemaphoreGive(uart_mutex);
    return total;
}
