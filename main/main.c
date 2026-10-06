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
#include "portal_config.h"
#include "web_server.h"
#include "modbus_poll.h"
#include "terminal_tcp.h"

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

    // 5. DNS-перехват только в режиме captive portal
#if CAPTIVE_PORTAL_ENABLE
    dns_server_start();
    ESP_LOGI(TAG, "DNS (captive portal) started");
#else
    ESP_LOGI(TAG, "Captive portal DNS disabled (CAPTIVE_PORTAL_ENABLE=0)");
#endif

    // 6. Запуск веб-сервера
    web_server_start();
    ESP_LOGI(TAG, "Web server started");

    // 7. Запуск задачи терминала
    terminal_task_start();
    ESP_LOGI(TAG, "Terminal task started");

    // 8. Raw TCP bridge for IRZ-Terminal (port 8888), optional; does not affect WS portal
    terminal_tcp_start();
    ESP_LOGI(TAG, "Terminal TCP bridge started (port %d)", TERMINAL_TCP_PORT);

    ESP_LOGI(TAG, "=== ALL SYSTEMS GO ===");
    ESP_LOGI(TAG, "Free heap: %d", esp_get_free_heap_size());
}