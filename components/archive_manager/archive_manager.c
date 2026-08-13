#include "archive_manager.h"
#include "web_server.h"
#include "modbus_master.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TAG              "ARCH"
#define PROGRESS_PERIOD_US 100000
#define CHUNK_SIZE       2000
#define HEADER_SIZE      132   
#define HEADER_SPEC_SIZE 4
#define MAX_RETRIES      5

/* ===== Глобалы состояния загрузки ===== */
bool download_in_progress = false;
volatile bool download_stop_requested = false;

static uint32_t total_size = 0;
static uint32_t downloaded = 0;
static char archive_name[256] = "archive.bin";
static SemaphoreHandle_t archive_mutex = NULL;
static bool name_extracted = false;
static httpd_handle_t current_download_req = NULL;

/* ===== ОЧЕРЕДЬ ===== */
static QueueHandle_t send_queue = NULL;
static SemaphoreHandle_t queue_mutex = NULL;
static TaskHandle_t send_task_handle = NULL;

/* ===== ЗАДАЧА ОТПРАВКИ ===== */
static void archive_manager_send_task(void *pvParameters)
{
    chunk_queue_item_t item;
    
    while (1) {
        if (xQueueReceive(send_queue, &item, portMAX_DELAY) == pdTRUE) {
            if (current_download_req != NULL) {
                httpd_req_t *req = (httpd_req_t *)current_download_req;
                esp_err_t ret = httpd_resp_send_chunk(req, (const char *)item.data, item.len);
                
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Send failed: %d", ret);
                    download_stop_requested = true;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

/* ===== ДОБАВЛЕНИЕ В ОЧЕРЕДЬ ===== */
static bool enqueue_chunk(const uint8_t *data, uint16_t len, uint32_t offset)
{
    if (send_queue == NULL) return false;
    if (download_stop_requested) return false;
    
    chunk_queue_item_t item;
    memcpy(item.data, data, len);
    item.len = len;
    item.offset = offset;
    
    if (xQueueSend(send_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Queue full, retrying...");
        if (xQueueSend(send_queue, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "Failed to enqueue chunk");
            return false;
        }
    }
    return true;
}

/* ===== ИНИЦИАЛИЗАЦИЯ ===== */
void archive_manager_init(void)
{
    if (archive_mutex == NULL) {
        archive_mutex = xSemaphoreCreateMutex();
    }
    
    if (send_queue == NULL) {
        send_queue = xQueueCreate(QUEUE_SIZE, sizeof(chunk_queue_item_t));
    }
    
    if (queue_mutex == NULL) {
        queue_mutex = xSemaphoreCreateMutex();
    }
    
    if (send_task_handle == NULL) {
        xTaskCreatePinnedToCore(
            archive_manager_send_task,
            "archive_send",
            4096,
            NULL,
            5,
            &send_task_handle,
            0
        );
    }
}

bool archive_manager_get_archive_size(uint32_t *size)
{
    return modbus_read_archive_size(1, size);
}

bool archive_manager_is_downloading(void)
{
    return download_in_progress;
}

void archive_manager_stop_download(void)
{
    download_stop_requested = true;
    
    if (current_download_req != NULL) {
        httpd_req_t *req = (httpd_req_t *)current_download_req;
        httpd_resp_send_chunk(req, NULL, 0);
        current_download_req = NULL;
    }
}

static void send_browser_log(const char *msg)
{
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "{\"type\":\"log\",\"msg\":\"%s\"}", msg);
    web_server_send(log_msg);
}

/* ===== ИЗВЛЕЧЕНИЕ ИМЕНИ ===== */
static void extract_filename(uint8_t *data, uint16_t *len)
{
    if (*len < HEADER_SIZE) return;
    
    char utf8_name[256] = {0};

    char decoded_name[256] = {0};
    int i = 0;
    while (i < HEADER_SIZE - HEADER_SPEC_SIZE && (HEADER_SPEC_SIZE + i) < *len) {
        decoded_name[i] = data[HEADER_SPEC_SIZE + i];
        i++;
    }
    decoded_name[i] = '\0';
                
    if (decoded_name[0] == '_' || decoded_name[0] == '/') {
        memmove(decoded_name, decoded_name + 1, strlen(decoded_name));
    }
    if (strlen(decoded_name) == 0) {
        strcpy(decoded_name, "archive");
    }
                
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
                
    char final_name[300];
    char *ext = strrchr(decoded_name, '.');
    if (ext) {
        int base_len = (int)(ext - decoded_name);
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
                
    char name_msg[500];
    snprintf(name_msg, sizeof(name_msg),
        "{\"type\":\"fileName\",\"name\":\"%s\"}", utf8_name);
    web_server_send(name_msg);

    strncpy(archive_name, utf8_name, sizeof(archive_name) - 1);
    archive_name[sizeof(archive_name) - 1] = 0;
                
    if (*len > HEADER_SIZE) {
        memmove(data, data + HEADER_SIZE, *len - HEADER_SIZE);
        *len -= HEADER_SIZE;
    } else {
        *len = 0;
    }
}

/* ===== HTTP ОБРАБОТЧИК ===== */
esp_err_t archive_manager_http_download_handler(httpd_req_t *req)
{
    if (download_in_progress) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "Download already in progress");
        return ESP_OK;
    }

    if (!modbus_read_archive_size(1, &total_size) || total_size == 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "Archive empty or unavailable");
        return ESP_OK;
    }

    download_in_progress = true;
    download_stop_requested = false;
    downloaded = 0;
    name_extracted = false;
    archive_name[0] = '\0';
    current_download_req = req;

    // Заголовки
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Transfer-Encoding", "chunked");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");

    // Размер архива
    char size_msg[80];
    snprintf(size_msg, sizeof(size_msg),
             "{\"type\":\"archiveSize\",\"size\":%lu}",
             (unsigned long)total_size);
    web_server_send(size_msg);

    send_browser_log("Starting archive download");

    uint8_t chunk[CHUNK_SIZE + 4];
    uint16_t chunk_len = 0;
    uint32_t offset = 0;
    bool stop_download = false;
    int chunk_counter = 0;
    bool data_sent = false;

    while (!download_stop_requested && downloaded < total_size && !stop_download) {
        modbus_status_t status;
        for(int i = 0; i < MAX_RETRIES; i++){
            status = modbus_read_file_0x64(1, offset, CHUNK_SIZE, chunk, &chunk_len);
            if(status == MODBUS_OK || status == MODBUS_END_OF_FILE)
                break;  
            else{
                char log_buf[128];
                snprintf(log_buf, sizeof(log_buf), "Read error %d/%d, status=%d", 
                        i, MAX_RETRIES, status);
                send_browser_log(log_buf);
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        }
        if (status == MODBUS_END_OF_FILE) {
            send_browser_log("End of file reached");
            break;
        }
        
        if (status == MODBUS_OK && chunk_len > 0) {
            
            // Извлечение имени
            if (!name_extracted && offset == 0 && chunk_len >= HEADER_SIZE) {
                extract_filename(chunk, &chunk_len);
                name_extracted = true;
            }
            
            // Добавляем в очередь
            if (!enqueue_chunk(chunk, chunk_len, offset)) {
                send_browser_log("Failed to enqueue chunk, stopping");
                stop_download = true;
                break;
            }

            data_sent = true; 
            offset += CHUNK_SIZE;
            downloaded += chunk_len;
            chunk_counter++;
            
            // Прогресс каждые 5 чанков
            if (chunk_counter % 5 == 0) {
                char progress_msg[256];
                snprintf(progress_msg, sizeof(progress_msg),
                        "{\"type\":\"progress\",\"received\":%lu,\"total\":%lu,\"percent\":%.2f}",
                        (unsigned long)downloaded, (unsigned long)total_size,
                        (float)downloaded / total_size * 100.0f);
                web_server_send(progress_msg);
            }
            
            // Проверка очереди
            UBaseType_t queue_space = uxQueueSpacesAvailable(send_queue);
            if (queue_space < 5) {
                vTaskDelay(pdMS_TO_TICKS(10));
            } else {
                vTaskDelay(pdMS_TO_TICKS(1));
            }

            if (downloaded >= total_size) {
                send_browser_log("All data read");
                break;
            }
            
        } else {
            send_browser_log("Too many errors, stopping");
            stop_download = true;
            break;
        }
    }

    // Ждём опустошения очереди
    send_browser_log("Waiting for queue to empty...");
    int wait_count = 0;
    while (uxQueueMessagesWaiting(send_queue) > 0 && !download_stop_requested) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_count++;
        if (wait_count > 100) { // 1 секунда таймаут
            send_browser_log("Queue wait timeout");
            break;
        }
    }
    send_browser_log("Queue empty");

    // ЗАВЕРШЕНИЕ ОТВЕТА - ВСЕГДА отправляем финальный чанк
    if (download_stop_requested || stop_download) {
        send_browser_log("Download stopped by error or user");
        httpd_resp_send_chunk(req, NULL, 0);
    } else if (downloaded >= total_size) {
        send_browser_log("Sending final chunk - download complete");
        httpd_resp_send_chunk(req, NULL, 0);
    } else if (!data_sent) {
        send_browser_log("No data sent, sending empty response");
        httpd_resp_send_chunk(req, NULL, 0);
    } else {
        // Частичная загрузка - закрываем поток
        send_browser_log("Partial download, closing stream");
        httpd_resp_send_chunk(req, NULL, 0);
    }
    
    current_download_req = NULL;

    // Итоговое сообщение
    char msg[512];
    if (download_stop_requested || stop_download) {
        snprintf(msg, sizeof(msg),
                 "{\"type\":\"downloadStopped\",\"received\":%lu,\"fileName\":\"%s\"}",
                 (unsigned long)downloaded, archive_name);
        web_server_send(msg);
        char log_buf[128];
        snprintf(log_buf, sizeof(log_buf), "Download stopped at %lu bytes", (unsigned long)downloaded);
        send_browser_log(log_buf);
    } else if (downloaded >= total_size) {
        snprintf(msg, sizeof(msg),
                 "{\"type\":\"downloadComplete\",\"total\":%lu,\"fileName\":\"%s\"}",
                 (unsigned long)downloaded, archive_name);
        web_server_send(msg);
        send_browser_log("Download completed successfully");
    } else {
        snprintf(msg, sizeof(msg),
                 "{\"type\":\"downloadStopped\",\"received\":%lu,\"total\":%lu,\"fileName\":\"%s\"}",
                 (unsigned long)downloaded, (unsigned long)total_size, archive_name);
        web_server_send(msg);
    }

    download_in_progress = false;
    download_stop_requested = false;

    return ESP_OK;
}

/* ===== WEB SOCKET КОМАНДЫ ===== */
void archive_manager_on_ws_command(const char *cmd)
{
    if (cmd == NULL) return;

    if (strstr(cmd, "\"action\":\"getArchiveSize\"")) {
        uint32_t size = 0;
        if (modbus_read_archive_size(1, &size)) {
            char resp[80];
            snprintf(resp, sizeof(resp),
                     "{\"type\":\"archiveSize\",\"size\":%lu}",
                     (unsigned long)size);
            web_server_send(resp);
        } else {
            web_server_send("{\"type\":\"error\",\"msg\":\"Failed to read archive size\"}");
        }
        return;
    }

    if (strstr(cmd, "\"action\":\"startDownload\"")) {
        if (download_in_progress) {
            web_server_send("{\"type\":\"error\",\"msg\":\"Download already in progress\"}");
            return;
        }
        web_server_send("{\"type\":\"log\",\"msg\":\"Download start requested\"}");
        return;
    }

    if (strstr(cmd, "\"action\":\"stopDownload\"")) {
        if (download_in_progress) {
            download_stop_requested = true;
            web_server_send("{\"type\":\"log\",\"msg\":\"Download stopped by user\"}");
        } else {
            web_server_send("{\"type\":\"log\",\"msg\":\"No active download to stop\"}");
        }
        return;
    }
}