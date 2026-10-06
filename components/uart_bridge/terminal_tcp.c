/**
 * @file terminal_tcp.c
 * @brief Raw TCP bridge for IRZ-Terminal (optional mode).
 *
 * Protocol (port TERMINAL_TCP_PORT):
 *   ESP → App : continuous ANSI screen bytes (same payload as WS binary)
 *   App → ESP : native key bytes from IRZ (0x31 F1, 0x65 Enter, …)
 *               forwarded to terminal_send_command() WITHOUT get_key_code()
 *
 * Existing WebSocket captive-portal terminal is unchanged.
 */

#include "terminal_tcp.h"
#include "modbus_poll.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/err.h"

#include <string.h>
#include <errno.h>
#include <unistd.h>

static const char *TAG = "TERM_TCP";

static int s_listen_fd = -1;
static int s_client_fd = -1;
static SemaphoreHandle_t s_mutex = NULL;

bool terminal_tcp_client_connected(void)
{
    return s_client_fd >= 0;
}

void terminal_tcp_send(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return;
    }
    if (s_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int fd = s_client_fd;
    if (fd >= 0) {
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = send(fd, data + sent, len - sent, 0);
            if (n < 0) {
                ESP_LOGW(TAG, "send failed errno=%d, closing client", errno);
                close(fd);
                s_client_fd = -1;
                break;
            }
            if (n == 0) {
                break;
            }
            sent += (size_t)n;
        }
    }
    xSemaphoreGive(s_mutex);
}

static void close_client_locked(void)
{
    if (s_client_fd >= 0) {
        close(s_client_fd);
        s_client_fd = -1;
        ESP_LOGI(TAG, "client disconnected");
    }
}

/**
 * @brief Handle one client: read key bytes and forward to Modbus 0x65.
 *        Screen is pushed from terminal_task via terminal_tcp_send().
 */
static void handle_client(int fd)
{
    uint8_t buf[64];

    ESP_LOGI(TAG, "IRZ client connected fd=%d", fd);

    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }

        /* IRZ sends 1-byte (or short) native commands; pass as C-string to 0x65 */
        if (n > 252) {
            n = 252;
        }
        char key[256];
        memcpy(key, buf, (size_t)n);
        key[n] = '\0';

        if (!terminal_send_command(1, key)) {
            ESP_LOGW(TAG, "terminal_send_command failed (len=%d)", (int)n);
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_client_fd == fd) {
        close_client_locked();
    } else {
        close(fd);
    }
    xSemaphoreGive(s_mutex);
}

static void terminal_tcp_task(void *arg)
{
    (void)arg;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(TERMINAL_TCP_PORT);

    s_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s_listen_fd < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(s_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind() port %d failed: %d", TERMINAL_TCP_PORT, errno);
        close(s_listen_fd);
        s_listen_fd = -1;
        vTaskDelete(NULL);
        return;
    }

    if (listen(s_listen_fd, 1) != 0) {
        ESP_LOGE(TAG, "listen() failed: %d", errno);
        close(s_listen_fd);
        s_listen_fd = -1;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "listening on 0.0.0.0:%d (IRZ-Terminal bridge)", TERMINAL_TCP_PORT);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int fd = accept(s_listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (fd < 0) {
            ESP_LOGW(TAG, "accept failed: %d", errno);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        /* Only one client: replace previous */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_client_fd >= 0) {
            ESP_LOGI(TAG, "replacing previous client fd=%d", s_client_fd);
            close(s_client_fd);
            s_client_fd = -1;
        }
        s_client_fd = fd;
        xSemaphoreGive(s_mutex);

        /* Optional: disable Nagle for lower latency on short key packets */
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        handle_client(fd);
    }
}

void terminal_tcp_start(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }

    BaseType_t ok = xTaskCreate(
        terminal_tcp_task,
        "term_tcp",
        4096,
        NULL,
        5,
        NULL
    );
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create term_tcp task");
    }
}
