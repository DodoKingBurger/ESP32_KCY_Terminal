#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

#include "uart_bridge.h"
#include "wifi_manager.h"
#include "dns_server.h"
#include "web_server.h"
#include "modbus_poll.h"

static const char *TAG = "MAIN";

/**
 * @brief Главная точка входа приложения.
 *        Инициализирует NVS, UART-мост, Wi-Fi точку доступа,
 *        DNS-сервер, веб-сервер и задачу опроса терминала.
 */
void app_main(void)
{
    ESP_LOGI(TAG, "=== APPLICATION START ===");
    
    // 1. Инициализация NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGI(TAG, "Erasing NVS...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 3. Инициализация UART
    uart_bridge_init();
    ESP_LOGI(TAG, "UART bridge initialized");

    // 4. Запуск WiFi
    wifi_manager_start();
    ESP_LOGI(TAG, "WiFi started");

    // 5. Запуск DNS
    dns_server_start();
    ESP_LOGI(TAG, "DNS started");

    // 6. Запуск веб-сервера
    web_server_start();
    ESP_LOGI(TAG, "Web server started");

    // 7. Запуск задачи терминала
    terminal_task_start();
    ESP_LOGI(TAG, "Terminal task started");

    ESP_LOGI(TAG, "=== ALL SYSTEMS GO ===");
    ESP_LOGI(TAG, "Free heap: %d", esp_get_free_heap_size());
}