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

/**
 * @brief Главная точка входа приложения.
 *        Инициализирует NVS, UART-мост, Wi-Fi точку доступа,
 *        DNS-сервер, веб-сервер и задачу опроса терминала.
 */
void app_main(void)
{
    nvs_flash_init();
    
    uart_bridge_init();

    wifi_manager_start();

    dns_server_start();

    web_server_start();

    terminal_task_start();
}