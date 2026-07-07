/* UART Echo Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_log.h"

#include "nvs_flash.h"
#include "uart_bridge.h"
#include "wifi_manager.h"
#include "dns_server.h"
#include "web_server.h"
#include "modbus_poll.h"
/**
 * This is an example which echos any data it receives on configured UART back to the sender,
 * with hardware flow control turned off. It does not use UART driver event queue.
 *
 * - Port: configured UART
 * - Receive (Rx) buffer: on
 * - Transmit (Tx) buffer: off
 * - Flow control: off
 * - Event queue: off
 * - Pin assignment: see defines below (See Kconfig)
 */

#define ECHO_TEST_TXD (CONFIG_EXAMPLE_UART_TXD)
#define ECHO_TEST_RXD (CONFIG_EXAMPLE_UART_RXD)
#define ECHO_TEST_RTS (UART_PIN_NO_CHANGE)
#define ECHO_TEST_CTS (UART_PIN_NO_CHANGE)

#define ECHO_UART_PORT_NUM      (CONFIG_EXAMPLE_UART_PORT_NUM)
#define ECHO_UART_BAUD_RATE     (CONFIG_EXAMPLE_UART_BAUD_RATE)
#define ECHO_TASK_STACK_SIZE    (CONFIG_EXAMPLE_TASK_STACK_SIZE)

static const char *TAG = "UART TEST\n";


//static const uint8_t tx_bytes[] = { 0x01, 0x03, 0x01, 0x00, 0x00, 0x01, 0x85, 0xF6 };
//static const size_t tx_len = sizeof(tx_bytes);

#define BUF_SIZE (1024)

static void echo_task(void *arg)
{
    /* Configure parameters of an UART driver,
     * communication pins and install the driver */
    uart_config_t uart_config = {
        .baud_rate = ECHO_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    int intr_alloc_flags = 0;

#if CONFIG_UART_ISR_IN_IRAM
    intr_alloc_flags = ESP_INTR_FLAG_IRAM;
#endif

    ESP_ERROR_CHECK(uart_driver_install(ECHO_UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(ECHO_UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(ECHO_UART_PORT_NUM, ECHO_TEST_TXD, ECHO_TEST_RXD, ECHO_TEST_RTS, ECHO_TEST_CTS));

    // Configure a temporary buffer for the incoming data
    uint8_t *data = (uint8_t *) malloc(BUF_SIZE);

    char current_msg[64] = "Hello";
    uint8_t rx_buffer[128];

    uint8_t tx_bytes[] = { 0x01, 0x03, 0x01, 0x00, 0x00, 0x01, 0x85, 0xF6 };
    //uint8_t tx_bytes[] = { 0x41, 0x42, 0x43, 0x0A };
    size_t tx_len = sizeof(tx_bytes);
    while (1) {
        // Отправка запроса
        int written = uart_write_bytes(ECHO_UART_PORT_NUM, tx_bytes, tx_len);
        if (written == tx_len) {
            ESP_LOGI(TAG, "Sent %d bytes", tx_len);
            ESP_LOG_BUFFER_HEX(TAG, tx_bytes, tx_len);  // Печать отправленных байтов
        } else {
            ESP_LOGE(TAG, "Failed to send, written %d", written);
        }

        // Чтение ответа (таймаут 200 мс)
        int len = uart_read_bytes(ECHO_UART_PORT_NUM, rx_buffer, sizeof(rx_buffer), pdMS_TO_TICKS(200));
        if (len > 0) {
            ESP_LOGI(TAG, "Received %d bytes:", len);
            ESP_LOG_BUFFER_HEX(TAG, rx_buffer, len);   // Покажет все байты ответа
        } else {
            ESP_LOGW(TAG, "No response (timeout)");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    free(data);
}

void app_main(void)
{
    nvs_flash_init();
    
    uart_bridge_init();

    wifi_manager_start();

    dns_server_start();

    web_server_start();

    terminal_task_start();
}