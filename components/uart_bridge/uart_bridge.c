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
        pdMS_TO_TICKS(200)
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