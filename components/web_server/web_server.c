
#include "web_server.h"
#include "modbus_master.h"
#include "archive_manager.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "WS";
bool load_page_active = false;
bool web_client_connected = false;
static SemaphoreHandle_t ws_mutex      = NULL;
static httpd_handle_t    server        = NULL;

static int     terminal_owner_fd  = -1;
//static int64_t terminal_last_ping = 0;

extern const uint8_t xterm_js_start[]   asm("_binary_xterm_js_start");
extern const uint8_t xterm_js_end[]     asm("_binary_xterm_js_end");
extern const uint8_t xterm_css_start[]  asm("_binary_xterm_css_start");
extern const uint8_t xterm_css_end[]    asm("_binary_xterm_css_end");
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_err_t root_get_handler     (httpd_req_t *req);
static esp_err_t ws_handler           (httpd_req_t *req);
static esp_err_t xterm_js_handler     (httpd_req_t *req);
static esp_err_t xterm_css_handler    (httpd_req_t *req);
static esp_err_t favicon_handler      (httpd_req_t *req);
static TaskHandle_t ws_ping_task_handle = NULL;

/*
// Функция задачи для обработки пингов
static void ws_ping_task(void *pvParameters)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000)); // Каждые 3 секунды
        //if (archive_manager_is_downloading()) {
            //continue;
        //}
        if (terminal_owner_fd >= 0 && server != NULL && web_client_connected) {
            // Отправляем пинг
            httpd_ws_frame_t ws_ping = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t *)"ping",
                .len = 4,
            };
            
            xSemaphoreTake(ws_mutex, portMAX_DELAY);
            esp_err_t ret = httpd_ws_send_frame_async(server, terminal_owner_fd, &ws_ping);
            xSemaphoreGive(ws_mutex);
            
            if (ret == ESP_OK ) {
                terminal_last_ping = esp_timer_get_time() / 1000;
            } else {
                // Если не можем отправить пинг - соединение разорвано
                terminal_owner_fd = -1;
                web_client_connected = false; // добавить
            }
        }
    }
}
    */

/**
 * @brief Отправляет текстовое сообщение через WebSocket текущему владельцу терминала
 * @param json строка JSON для отправки
 */
void web_server_send(const char *json)
{
    if (terminal_owner_fd < 0 || server == NULL || !web_client_connected || json == NULL) return;

    httpd_ws_frame_t ws = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len     = strlen(json),
    };

    xSemaphoreTake(ws_mutex, portMAX_DELAY);
    esp_err_t ret = httpd_ws_send_frame_async(server, terminal_owner_fd, &ws);
    xSemaphoreGive(ws_mutex);

    // Не отпускаем терминал и не делаем рекурсивный вызов при временной ошибке.
    // Реальный disconnect обрабатывается в ws_handler / client_disconnected.
    if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "ws_send_text failed: %s", esp_err_to_name(ret));
    }
}

/**
 * @brief Возвращает размер готового архива (делегирует archive_manager)
 * @return размер в байтах
 */
size_t web_server_get_archive_size(void)
{
    return archive_manager_get_size();
}

/**
 * @brief Обработчик GET /style.css – отдаёт встроенный CSS-файл.
 */
static esp_err_t style_css_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    extern const uint8_t style_css_start[]   asm("_binary_style_css_start");
    extern const uint8_t style_css_end[]     asm("_binary_style_css_end");
    return httpd_resp_send(req, (const char *)style_css_start,
        style_css_end - style_css_start);
}

/**
 * @brief Обработчик GET /script.js – отдаёт встроенный JS-файл.
 */
static esp_err_t script_js_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    extern const uint8_t script_js_start[]   asm("_binary_script_js_start");
    extern const uint8_t script_js_end[]     asm("_binary_script_js_end");
    return httpd_resp_send(req, (const char *)script_js_start,
        script_js_end - script_js_start);
}

/**
 * @brief Обработчик HTTP GET для корневого URI (/).
 *        Отдаёт HTML-страницу index.html с кэш-заголовками no-cache.
 * @param req указатель на запрос HTTP
 * @return ESP_OK или ESP_FAIL
 */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma",        "no-cache");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start,
        index_html_end - index_html_start);
}

/**
 * @brief Обработчик GET /xterm.js – отдаёт встроенный JS-файл xterm.
 * @param req указатель на запрос
 * @return ESP_OK
 */
static esp_err_t xterm_js_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    return httpd_resp_send(req, (const char *)xterm_js_start,
        xterm_js_end - xterm_js_start);
}

/**
 * @brief Обработчик GET /xterm.css – отдаёт встроенный CSS-файл xterm.
 * @param req указатель на запрос
 * @return ESP_OK
 */
static esp_err_t xterm_css_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    return httpd_resp_send(req, (const char *)xterm_css_start,
        xterm_css_end - xterm_css_start);
}

/**
 * @brief Обработчик GET /favicon.ico – возвращает HTTP 204 No Content.
 * @param req указатель на запрос
 * @return ESP_OK
 */
static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
}

/**
 * @brief Обработчик ошибки 404 – перенаправляет клиента на корень (http://192.168.4.1/).
 * @param req указатель на запрос
 * @param err код ошибки
 * @return ESP_OK
 */
static esp_err_t redirect_to_root(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

/**
 * @brief Вызывается при отключении WebSocket / Wi-Fi-клиента.
 *        Освобождает терминал. HTTP-скачивание архива НЕ останавливаем —
 *        оно идёт по отдельному TCP-соединению и не зависит от WS.
 */
void web_server_client_disconnected(void)
{   
    terminal_owner_fd  = -1;
    web_client_connected = false;
    // Намеренно НЕ вызываем archive_manager_stop_download().
    // Клиент закрывает WS перед скачиванием специально, чтобы
    // избежать обрывов SoftAP. HTTP-поток продолжает работать.
}

/**
 * @brief Обработчик WebSocket-соединений (/ws).
 *        Обрабатывает установку соединения (GET), текстовые команды (JSON),
 *        ping-сообщения и команды клавиатуры.
 * @param req указатель на запрос HTTP
 * @return ESP_OK или код ошибки
 */
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        /* Не отвечаем 503 — браузер получает обычный HTTP вместо WS-кадра
         * и пишет "Invalid frame header". Новый клиент просто забирает терминал. */
        if (terminal_owner_fd != -1 && terminal_owner_fd != fd) {
            ESP_LOGI(TAG, "Replacing terminal owner fd %d -> %d", terminal_owner_fd, fd);
        }
        terminal_owner_fd  = fd;
        web_client_connected = true;
        /* Не шлём web_server_send() здесь: handshake ещё не завершён. */
        ESP_LOGI(TAG, "WS owner connected fd=%d", fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        int fd = httpd_req_to_sockfd(req);
        if (fd == terminal_owner_fd) web_server_client_disconnected();
        if (frame.payload) free(frame.payload);
        return ret;
    }

    if (frame.len == 0) return ESP_OK;

    frame.payload = malloc(frame.len + 1);
    if (!frame.payload) return ESP_ERR_NO_MEM;

    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret != ESP_OK) { free(frame.payload); return ret; }
    ((char *)frame.payload)[frame.len] = 0;
    const char *cmd = (const char *)frame.payload;
    // Добавь это перед обработкой сообщений
    if (terminal_owner_fd < 0) {
        free(frame.payload);
        return ESP_OK;
    }
    //if (strcmp(cmd, "ping") == 0) {
    //    int fd = httpd_req_to_sockfd(req);
    //    if (fd == terminal_owner_fd) {
    //        terminal_last_ping = esp_timer_get_time() / 1000;
    //        // Отправляем pong
    //        httpd_ws_frame_t ws_pong = {
    //            .type = HTTPD_WS_TYPE_TEXT,
    //            .payload = (uint8_t *)"pong",
    //            .len = 4,
    //        };
    //        httpd_ws_send_frame_async(server, fd, &ws_pong);
    //    }
    //    free(frame.payload);
    //    return ESP_OK;
    //}

    //if (strcmp(cmd, "pong") == 0) {
        //int fd = httpd_req_to_sockfd(req);
        //if (fd == terminal_owner_fd) {
            //terminal_last_ping = esp_timer_get_time() / 1000;
        //}
        //free(frame.payload);
        //return ESP_OK;
    //}

    // Добавь это перед обработкой сообщений
    if (terminal_owner_fd < 0) {
        free(frame.payload);
        return ESP_OK;
    }
    if (cmd[0] == '{') {
        if (strstr(cmd, "\"action\":\"setTime\"")) {
            int year = 2026, month = 7, day = 6, hour = 13, minute = 51, second = 43;
            
            const char *p = strstr(cmd, "\"year\":");
            if (p) sscanf(p + 7, "%d", &year);
            p = strstr(cmd, "\"month\":");
            if (p) sscanf(p + 8, "%d", &month);
            p = strstr(cmd, "\"day\":");
            if (p) sscanf(p + 6, "%d", &day);
            p = strstr(cmd, "\"hour\":");
            if (p) sscanf(p + 7, "%d", &hour);
            p = strstr(cmd, "\"minute\":");
            if (p) sscanf(p + 9, "%d", &minute);
            p = strstr(cmd, "\"second\":");
            if (p) sscanf(p + 9, "%d", &second);
            
            struct tm timeinfo = {
                .tm_year = year - 1900,
                .tm_mon = month - 1,
                .tm_mday = day,
                .tm_hour = hour,
                .tm_min = minute,
                .tm_sec = second
            };
            time_t t = mktime(&timeinfo);
            struct timeval now = { .tv_sec = t, .tv_usec = 0 };
            settimeofday(&now, NULL);
            
            web_server_send("{\"type\":\"log\",\"msg\":\"Time updated from browser\"}");
            free(frame.payload);
            return ESP_OK;
        }
        if (strstr(cmd, "\"action\":\"setTerminalActive\"")) {
            if (strstr(cmd, "\"active\":true")) {
                load_page_active = true;
            } else {
                load_page_active = false;
            }
            free(frame.payload);
            return ESP_OK;
        }
        // ===== ИСПОЛЬЗУЕМ archive_manager_on_ws_command =====
        archive_manager_on_ws_command(cmd);
        free(frame.payload);
        return ESP_OK;
    }

    const char *mapped = get_key_code(cmd);
    if (mapped != NULL) {
        terminal_send_command(1, mapped);
    } else {
        web_server_send("{\"type\":\"log\",\"msg\":\"Unknown command: %s\"}");
    }
    free(frame.payload);
    return ESP_OK;
}

/**
 * @brief Отправляет бинарные данные (экран терминала) через WebSocket.
 * @param data указатель на данные
 * @param len  длина данных
 */
void web_server_send_binary(const uint8_t *data, size_t len)
{
    if (server == NULL || terminal_owner_fd < 0 || !web_client_connected || data == NULL || len == 0) return;

    httpd_ws_frame_t ws_pkt = {
        .final      = true,
        .fragmented = false,
        .type       = HTTPD_WS_TYPE_BINARY,
        .payload    = (uint8_t *)data,
        .len        = len,
    };

    xSemaphoreTake(ws_mutex, portMAX_DELAY);
    esp_err_t ret = httpd_ws_send_frame_async(server, terminal_owner_fd, &ws_pkt);
    xSemaphoreGive(ws_mutex);

    // Не сбрасываем владельца при единичной ошибке — иначе терминал «умирает»
    // от любого временного сбоя SoftAP. Реальный disconnect придёт через ws_handler.
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ws_send_binary failed: %s", esp_err_to_name(ret));
    }
}

/**
 * @brief Запускает HTTP-сервер, инициализирует archive_manager,
 *        регистрирует все URI-обработчики и обработчик ошибки 404.
 */
void web_server_start(void)
{
    if (ws_mutex == NULL) ws_mutex = xSemaphoreCreateMutex();
    
    // ===== ИНИЦИАЛИЗИРУЕМ archive_manager =====
    archive_manager_init();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port          = 80;
    config.max_open_sockets     = 7;
    config.lru_purge_enable     = true;
    /* send timeout 5с: при abort быстро выходим; при медленном SoftAP ещё терпимо */
    config.recv_wait_timeout    = 10;
    config.send_wait_timeout    = 5;
    config.keep_alive_enable    = false;
    config.max_uri_handlers     = 20;
    config.stack_size           = 12288;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE("HTTP", "Server start failed");
        return;
    }
    ESP_LOGI("HTTP", "Server started http://192.168.4.1");
    httpd_register_uri_handler(server, &(httpd_uri_t){ 
        .uri = "/download",  
        .method = HTTP_GET, 
        .handler = archive_manager_http_download_handler 
    });
    httpd_register_uri_handler(server, &(httpd_uri_t){
        .uri = "/stop-download",
        .method = HTTP_GET,
        .handler = archive_manager_http_stop_handler
    });
    httpd_register_uri_handler(server, &(httpd_uri_t){
        .uri = "/stop-download",
        .method = HTTP_POST,
        .handler = archive_manager_http_stop_handler
    });

    httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/style.css", .method = HTTP_GET, .handler = style_css_handler });
    httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/script.js", .method = HTTP_GET, .handler = script_js_handler });

    httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/",          .method = HTTP_GET, .handler = root_get_handler });
    httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/xterm.js",  .method = HTTP_GET, .handler = xterm_js_handler });
    httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/xterm.css", .method = HTTP_GET, .handler = xterm_css_handler });
    httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/ws",        .method = HTTP_GET, .handler = ws_handler,        .is_websocket = true });

    httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler });
    //Captive portal переадресация
    //httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/generate_204", .method = HTTP_GET, .handler = root_get_handler });
    //httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = root_get_handler });
    //httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/library/test/success.html", .method = HTTP_GET, .handler = root_get_handler });

    //httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_to_root);
    //if (ws_ping_task_handle == NULL) {
    //    xTaskCreatePinnedToCore(
    //        ws_ping_task,
    //        "ws_ping",
    //        2048,
    //        NULL,
    //        10,  // Приоритет выше, чем у HTTP
    //        &ws_ping_task_handle,
    //        0
    //    );
    //}
    web_client_connected = true;
    ESP_LOGI("HTTP", "Captive portal enabled");
}