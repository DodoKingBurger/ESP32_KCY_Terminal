#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"
#include "lwip/inet.h"
#include "web_server.h"
#include <string.h>

#define AP_SSID "ESP32-Portal"
#define AP_PASS "12345678"

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    switch (event_id) {
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI("WIFI", "Client connected");
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI("WIFI", "Client disconnected");
            web_server_client_disconnected();
            break;
        default:
            break;
    }
}

void wifi_manager_start(void)
{
    // Инициализация сетевого интерфейса и цикла событий (уже сделано в main, но для надёжности)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // --- 1. Создаём сетевой интерфейс для точки доступа ДО инициализации Wi-Fi ---
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    assert(ap_netif);

    // --- 2. Настраиваем IP-адрес, маску, шлюз и DNS ---
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));

    esp_netif_dns_info_t dns;
    IP4_ADDR(&dns.ip.u_addr.ip4, 192, 168, 4, 1);
    dns.ip.type = IPADDR_TYPE_V4;
    ESP_ERROR_CHECK(esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns));

    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

    // --- 3. Инициализация стека Wi-Fi ---
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Регистрируем обработчик событий
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));

    // --- 4. Принудительный сброс сохранённой конфигурации (избегаем конфликтов) ---
    ESP_ERROR_CHECK(esp_wifi_restore());

    // --- 5. Настройка режима и параметров точки доступа ---
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = AP_SSID,
            .password = AP_PASS,
            .ssid_len = strlen(AP_SSID),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        }
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // --- 6. Запуск Wi-Fi ---
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI("WIFI", "Access point started with SSID: %s", AP_SSID);
}