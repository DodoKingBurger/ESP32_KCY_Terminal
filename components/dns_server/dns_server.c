
#include "dns_server.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#define DNS_PORT 53


static const char *TAG = "DNS";

/**
 * @brief Задача DNS-сервера (отвечает на все A-запросы адресом 192.168.4.1)
 */
static void dns_task(void *arg)
{
    int sock;

    struct sockaddr_in server_addr;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);

    if (sock < 0)
    {
        ESP_LOGE(TAG, "Socket create failed");
        vTaskDelete(NULL);
        return;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(DNS_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0)
    {
        ESP_LOGE(TAG, "Bind failed");

        close(sock);

        vTaskDelete(NULL);

        return;
    }

    ESP_LOGI(TAG, "DNS server started");

    while (1)
    {
        uint8_t rx_buffer[512];

        struct sockaddr_in client_addr;

        socklen_t addr_len =
            sizeof(client_addr);

        int len = recvfrom(
            sock,
            rx_buffer,
            sizeof(rx_buffer),
            0,
            (struct sockaddr *)&client_addr,
            &addr_len
        );

        if (len <= 0 || len < 12)
            continue;

        ESP_LOGI(
            TAG,
            "DNS request len=%d from %s",
            len,
            inet_ntoa(client_addr.sin_addr)
        );

        int pos = 12;

        while (pos < len && rx_buffer[pos] != 0)
        {
            pos += rx_buffer[pos] + 1;
        }

        pos++;

        if (pos + 4 > len)
            continue;

        uint16_t qtype =
            (rx_buffer[pos] << 8) |
            rx_buffer[pos + 1];

        if (qtype != 1)
        {
            /* Только A-запросы */

            continue;
        }

        /*
         * DNS response
         */

        uint8_t response[512];

        memcpy(response, rx_buffer, len);

        response[2] = 0x81;
        response[3] = 0x80;

        response[7] = 0x01;

        pos = len;

        /*
         * Answer section
         */

        response[pos++] = 0xC0;
        response[pos++] = 0x0C;

        response[pos++] = 0x00;
        response[pos++] = 0x01;

        response[pos++] = 0x00;
        response[pos++] = 0x01;

        response[pos++] = 0x00;
        response[pos++] = 0x00;
        response[pos++] = 0x00;
        response[pos++] = 0x3C;

        response[pos++] = 0x00;
        response[pos++] = 0x04;

        /*
         * 192.168.4.1
         */

        response[pos++] = 192;
        response[pos++] = 168;
        response[pos++] = 4;
        response[pos++] = 1;

        sendto(
            sock,
            response,
            pos,
            0,
            (struct sockaddr *)&client_addr,
            sizeof(client_addr)
        );
    }
}

/**
 * @brief Запуск DNS-сервера в отдельной задаче
 */
void dns_server_start(void)
{
    xTaskCreate(
        dns_task,
        "dns_task",
        4096,
        NULL,
        5,
        NULL
    );
}