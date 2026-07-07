#include "web_server.h"
#include "modbus_master.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>    // <-- добавить
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define RECORD_LIMIT 0x270F

static const char *TAG = "WS";

static SemaphoreHandle_t ws_mutex      = NULL;
static SemaphoreHandle_t archive_mutex = NULL;
static httpd_handle_t    server        = NULL;

static uint32_t      download_total  = 0;
static uint32_t      download_offset = 0;
static TaskHandle_t  download_task_handle = NULL;

bool download_in_progress = false;
volatile bool download_stop_requested = false;

static int     terminal_owner_fd  = -1;
static int64_t terminal_last_ping = 0;

static uint8_t *archive_buf  = NULL;
static size_t   archive_size = 0;
static char     archive_name[256] = "archive.bin";

extern const uint8_t xterm_js_start[]   asm("_binary_xterm_js_start");
extern const uint8_t xterm_js_end[]     asm("_binary_xterm_js_end");
extern const uint8_t xterm_css_start[]  asm("_binary_xterm_css_start");
extern const uint8_t xterm_css_end[]    asm("_binary_xterm_css_end");
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_err_t root_get_handler     (httpd_req_t *req);
static esp_err_t ws_handler           (httpd_req_t *req);
static esp_err_t download_get_handler (httpd_req_t *req);
static esp_err_t xterm_js_handler     (httpd_req_t *req);
static esp_err_t xterm_css_handler    (httpd_req_t *req);
static esp_err_t favicon_handler      (httpd_req_t *req);
static void     download_task         (void *arg);

static void ws_send_text(const char *json)
{
    if (terminal_owner_fd < 0 || server == NULL) return;

    httpd_ws_frame_t ws = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len     = strlen(json),
    };

    xSemaphoreTake(ws_mutex, portMAX_DELAY);
    esp_err_t ret = httpd_ws_send_frame_async(server, terminal_owner_fd, &ws);
    xSemaphoreGive(ws_mutex);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ws_send_text failed, releasing terminal");
        terminal_owner_fd = -1;
        // Если загрузка активна – останавливаем
        if (download_in_progress) {
            download_stop_requested = true;
        }
    }
}

void web_server_claim_archive(uint8_t *data, size_t size, const char *name)
{
    if (archive_mutex == NULL) archive_mutex = xSemaphoreCreateMutex();
    xSemaphoreTake(archive_mutex, portMAX_DELAY);
    // Освобождаем старый буфер, если есть
    if (archive_buf) {
        free(archive_buf);
        archive_buf = NULL;
    }
    archive_size = 0;
    if (data && size > 0) {
        archive_buf = data;      // просто присваиваем указатель
        archive_size = size;
    }
    if (name) {
        strncpy(archive_name, name, sizeof(archive_name) - 1);
        archive_name[sizeof(archive_name) - 1] = 0;
    } else {
        archive_name[0] = 0;
    }
    xSemaphoreGive(archive_mutex);
}

void web_server_release_archive(void) { web_server_claim_archive (NULL, 0, NULL); }

size_t web_server_get_archive_size(void)
{
    if (archive_mutex == NULL) return 0;
    xSemaphoreTake(archive_mutex, portMAX_DELAY);
    size_t s = archive_size;
    xSemaphoreGive(archive_mutex);
    return s;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma",        "no-cache");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

static esp_err_t xterm_js_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    return httpd_resp_send(req, (const char *)xterm_js_start,
                           xterm_js_end - xterm_js_start);
}

static esp_err_t xterm_css_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    return httpd_resp_send(req, (const char *)xterm_css_start,
                           xterm_css_end - xterm_css_start);
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t redirect_to_root(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

void web_server_client_disconnected(void)
{
    terminal_owner_fd  = -1;
    terminal_last_ping = 0;
    // Останавливаем загрузку, если она активна
    if (download_in_progress) {
        download_stop_requested = true;
        ESP_LOGI(TAG, "Download stop requested due to client disconnect");
    }
    ESP_LOGI(TAG, "Terminal released");
}

void web_server_release_terminal(void) { terminal_owner_fd = -1; }

static esp_err_t download_get_handler(httpd_req_t *req)
{
    if (archive_mutex == NULL) archive_mutex = xSemaphoreCreateMutex();
    xSemaphoreTake(archive_mutex, portMAX_DELAY);
    uint8_t *buf = archive_buf;
    size_t size = archive_size;
    const char *name = archive_name[0] ? archive_name : "archive.bin";
    archive_buf = NULL;
    archive_size = 0;
    xSemaphoreGive(archive_mutex);

    if (!buf || size == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Archive empty");
        return ESP_OK;
    }

    char disposition[300];
    snprintf(disposition, sizeof(disposition),
             "attachment; filename=\"%s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    httpd_resp_set_type(req, "application/octet-stream");
    char len_hdr[32];
    snprintf(len_hdr, sizeof(len_hdr), "%u", (unsigned)size);
    httpd_resp_set_hdr(req, "Content-Length", len_hdr);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    const size_t CHUNK = 4096;
    size_t sent = 0;
    while (sent < size) {
        size_t n = (size - sent > CHUNK) ? CHUNK : (size - sent);
        if (httpd_resp_send_chunk(req, (const char *)(buf + sent), n) != ESP_OK) {
            return ESP_FAIL;
        }
        sent += n;
    }
    free(buf);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        if (terminal_owner_fd != -1) {
            int64_t now = esp_timer_get_time() / 1000;
            if ((now - terminal_last_ping) > 15000) {
                ESP_LOGW(TAG, "Owner timeout");
                terminal_owner_fd = -1;
            }
        }
        if (terminal_owner_fd != -1) {
            httpd_resp_set_status(req, "503 Service Unavailable");
            return httpd_resp_sendstr(req, "Terminal busy");
        }
        terminal_owner_fd  = fd;
        terminal_last_ping = esp_timer_get_time() / 1000;
        ESP_LOGI(TAG, "Owner connected fd=%d", fd);
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

    if (cmd[0] == '{') {
        if (strstr(cmd, "\"action\":\"setTime\"")) {
            // Парсим JSON
            // Простой способ: ищем значения вручную
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
            
            // Устанавливаем время
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
            
            ws_send_text("{\"type\":\"log\",\"msg\":\"Time updated from browser\"}");
            free(frame.payload);
            return ESP_OK;
        }
        if (strstr(cmd, "\"action\":\"getArchiveSize\"")) {
            uint32_t size = 0;
            if (modbus_read_archive_size(1, &size)) {
                char resp[80];
                snprintf(resp, sizeof(resp),
                         "{\"type\":\"archiveSize\",\"size\":%lu}",
                         (unsigned long)size);
                ws_send_text(resp);
            } else {
                ws_send_text("{\"type\":\"error\",\"msg\":\"Failed to read archive size\"}");
            }
            free(frame.payload);
            return ESP_OK;
        }
        if (strstr(cmd, "\"action\":\"startDownload\"")) {
            if (download_in_progress) {
                ws_send_text("{\"type\":\"error\",\"msg\":\"Download already in progress\"}");
                free(frame.payload);
                return ESP_OK;
            }
            download_in_progress    = true;
            download_stop_requested = false;
            download_offset         = 0;
            download_total          = 0;
            web_server_release_archive();
            terminal_last_ping      = esp_timer_get_time() / 1000;
            if (download_task_handle == NULL) {
                xTaskCreatePinnedToCore(
                    download_task, "download", 8192, NULL, 4,
                    &download_task_handle, tskNO_AFFINITY);
            }
            free(frame.payload);
            return ESP_OK;
        }
        if (strstr(cmd, "\"action\":\"stopDownload\"")) {
            download_stop_requested = true;
            ws_send_text("{\"type\":\"log\",\"msg\":\"Stop requested\"}");
            free(frame.payload);
            return ESP_OK;
        }
        free(frame.payload);
        return ESP_OK;
    }

    if (strcmp(cmd, "ping") == 0) {
        int fd = httpd_req_to_sockfd(req);
        if (fd == terminal_owner_fd) {
            terminal_last_ping = esp_timer_get_time() / 1000;
        }
        free(frame.payload);
        return ESP_OK;
    }

    const char *mapped = get_key_code(cmd);
    if (mapped != NULL) {
        terminal_send_command(1, mapped);
    } else {
        ESP_LOGW(TAG, "Unknown command: %s", cmd);
    }
    free(frame.payload);
    return ESP_OK;
}

static void download_task(void *arg)
{
    char utf8_name[256] = {0};
    uint32_t total_size = 0;
    if (!modbus_read_archive_size(1, &total_size) || total_size == 0) {
        ws_send_text("{\"type\":\"error\",\"msg\":\"Cannot read archive size\"}");
        goto cleanup;
    }
    if (total_size > 4u * 1024u * 1024u) {
        ws_send_text("{\"type\":\"error\",\"msg\":\"Archive too large for RAM (>4MB)\"}");
        goto cleanup;
    }

    download_total = total_size;
    char size_msg[80];
    snprintf(size_msg, sizeof(size_msg),
             "{\"type\":\"archiveSize\",\"size\":%lu}",
             (unsigned long)total_size);
    ws_send_text(size_msg);

    uint8_t *buf = (uint8_t *)malloc(total_size);
    if (!buf) {
        ws_send_text("{\"type\":\"error\",\"msg\":\"Out of memory\"}");
        goto cleanup;
    }
    size_t   written       = 0;
    bool     end_reached   = false;
    bool     first_packet  = true;
    int64_t  last_progress_us = 0;

    while (!download_stop_requested && !end_reached) {
        if (terminal_owner_fd < 0) {
            ESP_LOGW(TAG, "Client disconnected, stopping download");
            download_stop_requested = true;
            break;
        }

        if (total_size - written == 0) break;

        uint16_t file_id = 1 + (download_offset / 2 / (RECORD_LIMIT + 1));
        uint16_t record_number = (download_offset / 2) % (RECORD_LIMIT + 1);
        uint8_t chunk[256] = {0};
        uint16_t len = 0;

        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; attempt++) {
            modbus_status_t s = modbus_read_file_0x14(1, file_id, record_number, chunk, &len);
            if (s == MODBUS_OK) ok = true;
            else vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (!ok) {
            ws_send_text("{\"type\":\"error\",\"msg\":\"Failed to read data after retries\"}");
            free(buf);
            goto cleanup;
        }
        if (len == 0) { end_reached = true; break; }

        uint16_t original_len = len;
        // === ИЗВЛЕКАЕМ ИМЯ ИЗ ПЕРВОГО ПАКЕТА ===
        if (first_packet && len >= 132) {
            // 1. Копируем 128 байт (байты 4..131) — байты перевёрнуты
            uint8_t raw_name[128] = {0};
            for (int i = 0; i < 128 && (4 + i) < len; i++) {
                raw_name[i] = chunk[4 + i];
            }
            
            // 2. ПЕРЕВОРАЧИВАЕМ БАЙТЫ ИМЕНИ В ПАРАХ
            for (int i = 0; i < 127; i += 2) {
                uint8_t tmp = raw_name[i];
                raw_name[i] = raw_name[i + 1];
                raw_name[i + 1] = tmp;
            }
            
            // 3. Копируем имя до первого нулевого байта
            char decoded_name[256] = {0};
            int name_len = 0;
            while (name_len < 128 && raw_name[name_len] != 0) {
                decoded_name[name_len] = (char)raw_name[name_len];
                name_len++;
            }
            decoded_name[name_len] = '\0';
            
            // ===== ИСПРАВЛЕНИЕ: убираем только первый символ, если это _ или / =====
            // Убираем первый символ, если он '_' или '/'
            if (decoded_name[0] == '_' || decoded_name[0] == '/' ) {
                memmove(decoded_name, decoded_name + 1, strlen(decoded_name));
            }
            
            // Если имя стало пустым
            if (strlen(decoded_name) == 0) {
                strcpy(decoded_name, "archive");
            }
            
            // 4. Добавляем дату и время
            time_t now;
            struct tm timeinfo;
            time(&now);
            localtime_r(&now, &timeinfo);
            
            // 5. Формируем финальное имя
            char final_name[300];
            char *ext = strrchr(decoded_name, '.');
            if (ext) {
                int base_len = ext - decoded_name;
                snprintf(final_name, sizeof(final_name),
                        "%.*s %d_%d_%d %dч%02dм%02dс%s",
                        base_len, decoded_name,
                        timeinfo.tm_year + 1900,
                        timeinfo.tm_mon + 1,
                        timeinfo.tm_mday,
                        timeinfo.tm_hour,
                        timeinfo.tm_min,
                        timeinfo.tm_sec,
                        ext);
            } else {
                snprintf(final_name, sizeof(final_name),
                        "%s %d_%d_%d %dч%02dм%02dс.irz",
                        decoded_name,
                        timeinfo.tm_year + 1900,
                        timeinfo.tm_mon + 1,
                        timeinfo.tm_mday,
                        timeinfo.tm_hour,
                        timeinfo.tm_min,
                        timeinfo.tm_sec);
            }
            
            strncpy(utf8_name, final_name, sizeof(utf8_name) - 1);
            utf8_name[sizeof(utf8_name) - 1] = 0;
            
            // 6. Отправляем клиенту
            char name_msg[500];
            snprintf(name_msg, sizeof(name_msg),
                    "{\"type\":\"fileName\",\"name\":\"%s\"}", utf8_name);
            ws_send_text(name_msg);
            
            // 7. Удаляем первые 132 байта из чанка
            if (len > 132) {
                memmove(chunk, chunk + 132, len - 132);
                len -= 132;
            } else {
                len = 0;
            }
            
            first_packet = false;
        }

        // Копируем данные (БЕЗ ПЕРЕВОРОТА)
        if (written + len > total_size) {
            len = (uint16_t)(total_size - written);
            end_reached = true;
        }
        memcpy(buf + written, chunk, len);
        written += len;
        download_offset += original_len;
        
        // Прогресс
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_progress_us > 100000) {
            last_progress_us = now_us;
            float pct = (float)written / (float)total_size * 100.0f;
            char pm[160];
            snprintf(pm, sizeof(pm),
                     "{\"type\":\"progress\",\"received\":%lu,\"total\":%lu,\"percent\":%.2f}",
                     (unsigned long)written, (unsigned long)total_size, pct);
            ws_send_text(pm);
        }
        terminal_last_ping = esp_timer_get_time() / 1000;
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (download_stop_requested){
        // Переворачиваем скачанную часть
        for (size_t i = 0; i + 1 < written; i += 2)
        {
            uint8_t tmp = buf[i];
            buf[i] = buf[i + 1];
            buf[i + 1] = tmp;
        }

        // Сохраняем кусок архива
        web_server_claim_archive(
            buf,
            written,
            utf8_name[0] ? utf8_name : "archive_part.bin");

        char stop_msg[700];
        snprintf(stop_msg,
                sizeof(stop_msg),
                "{\"type\":\"downloadStopped\","
                "\"received\":%lu,"
                "\"fileName\":\"%s\","
                "\"url\":\"/download\"}",
                (unsigned long)written,
                utf8_name[0] ? utf8_name : "archive_part.bin");

        ws_send_text(stop_msg);
    }else if (end_reached || written >= total_size) {
        // === ПРИМЕНЯЕМ ПЕРЕВОРОТ КО ВСЕМУ БУФЕРУ ===
        for (size_t i = 0; i < written - 1; i += 2) {
            uint8_t tmp = buf[i];
            buf[i] = buf[i + 1];
            buf[i + 1] = tmp;
        }
        
        web_server_claim_archive(buf, written, utf8_name[0] ? utf8_name : "archive.bin");
        char complete_msg[600];
        snprintf(complete_msg, sizeof(complete_msg),
                 "{\"type\":\"downloadComplete\",\"total\":%lu,\"fileName\":\"%s\",\"url\":\"/download\"}",
                 (unsigned long)written, utf8_name[0] ? utf8_name : "archive.bin");
        ws_send_text(complete_msg);
    } else {
        ws_send_text("{\"type\":\"error\",\"msg\":\"Download interrupted\"}");
        free(buf);
    }

cleanup:
    download_in_progress = false;
    download_task_handle = NULL;
    vTaskDelete(NULL);
}

void web_server_send_binary(const uint8_t *data, size_t len)
{
    if (server == NULL || terminal_owner_fd < 0) return;
    
    // Если идёт загрузка, не отправляем бинарные данные (терминал не нужен)
    if (download_in_progress) return;

    int64_t now = esp_timer_get_time() / 1000;
    if ((now - terminal_last_ping) > 15000) {
        ESP_LOGW(TAG, "Owner timeout");
        terminal_owner_fd = -1;
        return;
    }

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

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Owner disconnected");
        terminal_owner_fd = -1;
        // Если загрузка активна – останавливаем
        if (download_in_progress) {
            download_stop_requested = true;
        }
    }
}

void web_server_send(const char *data) { ws_send_text(data); }

void web_server_start(void)
{
    if (ws_mutex      == NULL) ws_mutex      = xSemaphoreCreateMutex();
    if (archive_mutex == NULL) archive_mutex = xSemaphoreCreateMutex();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port          = 80;
    config.max_open_sockets     = 6;
    config.lru_purge_enable     = true;
    config.recv_wait_timeout    = 5;
    config.send_wait_timeout    = 5;
    config.keep_alive_enable    = true;
    config.keep_alive_idle      = 5;
    config.keep_alive_interval  = 3;
    config.keep_alive_count     = 3;
    config.max_uri_handlers     = 20;
    config.stack_size           = 12288;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE("HTTP", "Server start failed");
        return;
    }
    ESP_LOGI("HTTP", "Server started http://192.168.4.1");

    httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/",          .method = HTTP_GET, .handler = root_get_handler });
    httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/xterm.js",  .method = HTTP_GET, .handler = xterm_js_handler });
    httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/xterm.css", .method = HTTP_GET, .handler = xterm_css_handler });
    httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/ws",        .method = HTTP_GET, .handler = ws_handler,        .is_websocket = true });
    httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/download",  .method = HTTP_GET, .handler = download_get_handler });

    httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler });
    httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/generate_204", .method = HTTP_GET, .handler = root_get_handler });
    httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = root_get_handler });
    httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/library/test/success.html", .method = HTTP_GET, .handler = root_get_handler });

    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_to_root);
    ESP_LOGI("HTTP", "Captive portal enabled");
}