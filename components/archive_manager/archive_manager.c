
#include "archive_manager.h"
#include "web_server.h"
#include "modbus_master.h"

#include "esp_log.h"
#include "esp_http_server.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TAG              "ARCH"

bool download_in_progress = false;
volatile bool download_stop_requested = false;

static uint32_t total_size = 0;
static uint32_t downloaded = 0;
static char archive_name[256] = "archive.bin";
static SemaphoreHandle_t archive_mutex = NULL;
static bool name_extracted = false;

static volatile uint32_t download_generation = 0;

static void send_browser_log(const char *msg)
{
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "{\"type\":\"log\",\"msg\":\"%s\"}", msg);
    web_server_send(log_msg);
}

static void url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char *hex = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 4 < dst_size; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[j++] = (char)c;
        } else if (c == ' ') {
            dst[j++] = '%';
            dst[j++] = '2';
            dst[j++] = '0';
        } else {
            dst[j++] = '%';
            dst[j++] = hex[c >> 4];
            dst[j++] = hex[c & 0x0F];
        }
    }
    dst[j] = '\0';
}

static void extract_filename(uint8_t *data, uint16_t *len)
{
    if (*len < HEADER_SIZE) return;

    char decoded_name[256] = {0};
    int i = 0;
    while (i < HEADER_SIZE - HEADER_SPEC_SIZE && (HEADER_SPEC_SIZE + i) < *len) {
        decoded_name[i] = (char)data[HEADER_SPEC_SIZE + i];
        i++;
    }
    decoded_name[i] = '\0';

    for (int k = 0; k < i; k++) {
        if ((unsigned char)decoded_name[k] < 0x20) {
            decoded_name[k] = '\0';
            break;
        }
    }

    if (decoded_name[0] == '_' || decoded_name[0] == '/') {
        memmove(decoded_name, decoded_name + 1, strlen(decoded_name) + 1);
    }
    for (char *p = decoded_name; *p; p++) {
        if (*p == '/' || *p == '\\') *p = '_';
    }
    if (decoded_name[0] == '\0') {
        strcpy(decoded_name, "archive");
    }

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char final_name[300];
    char *ext = strrchr(decoded_name, '.');
    if (ext && ext != decoded_name) {
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

    strncpy(archive_name, final_name, sizeof(archive_name) - 1);
    archive_name[sizeof(archive_name) - 1] = 0;

    char name_msg[500];
    snprintf(name_msg, sizeof(name_msg),
        "{\"type\":\"fileName\",\"name\":\"%s\"}", archive_name);
    web_server_send(name_msg);

    if (*len > HEADER_SIZE) {
        memmove(data, data + HEADER_SIZE, *len - HEADER_SIZE);
        *len -= HEADER_SIZE;
    } else {
        *len = 0;
    }
}

void archive_manager_init(void)
{
    if (archive_mutex == NULL) {
        archive_mutex = xSemaphoreCreateMutex();
    }
}

bool archive_manager_get_archive_size(uint32_t *size)
{
    return modbus_read_archive_size(size);
}

size_t archive_manager_get_size(void)
{
    uint32_t size = 0;
    if (modbus_read_archive_size(&size)) {
        return (size_t)size;
    }
    return 0;
}

void archive_manager_get_name(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    if (archive_mutex) xSemaphoreTake(archive_mutex, portMAX_DELAY);
    strncpy(out, archive_name, out_size - 1);
    out[out_size - 1] = '\0';
    if (archive_mutex) xSemaphoreGive(archive_mutex);
}

bool archive_manager_is_downloading(void)
{
    return download_in_progress;
}

void archive_manager_stop_download(void)
{
    download_stop_requested = true;
    download_generation++;
    send_browser_log("stop flag set");
}

esp_err_t archive_manager_http_stop_handler(httpd_req_t *req)
{
    archive_manager_stop_download();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

/**
 * Отправка чанка. При любой ошибке — stop, больше не трогаем сокет.
 * НЕ используем recv/MSG_PEEK на fd httpd — это ломает внутреннее состояние.
 */
static bool send_chunk_to_client(httpd_req_t *req, const char *data, size_t len)
{
    if (download_stop_requested) return false;

    esp_err_t ret = httpd_resp_send_chunk(req, data, len);
    if (ret != ESP_OK) {
        send_browser_log("Send failed, client gone");
        download_stop_requested = true;
        return false;
    }
    return true;
}

esp_err_t archive_manager_http_download_handler(httpd_req_t *req)
{
    if (download_in_progress) {
        download_stop_requested = true;
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_hdr(req, "Retry-After", "1");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_sendstr(req, "Download busy, retry");
        return ESP_OK;
    }

    if (!modbus_read_archive_size(&total_size) || total_size == 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_sendstr(req, "Archive empty or unavailable");
        return ESP_OK;
    }

    const uint32_t my_gen = download_generation;
    download_in_progress = true;
    download_stop_requested = false;
    downloaded = 0;
    name_extracted = false;
    strncpy(archive_name, "archive.bin", sizeof(archive_name) - 1);
    archive_name[sizeof(archive_name) - 1] = 0;

    send_browser_log("Starting archive download");

    {
        char size_msg[80];
        snprintf(size_msg, sizeof(size_msg),
                 "{\"type\":\"archiveSize\",\"size\":%lu}",
                 (unsigned long)total_size);
        web_server_send(size_msg);
    }

    uint8_t chunk[CHUNK_SIZE + 4];
    uint16_t chunk_len = 0;
    uint32_t offset = 0;
    bool aborted = false;
    
    int chunk_counter = 0;

    /* Первый чанк */
    {
        modbus_status_t status = MODBUS_ERR;
        for (int i = 0; i < MAX_RETRIES; i++) {
            if (download_stop_requested || download_generation != my_gen) break;
            status = modbus_read_file_0x64(0, CHUNK_SIZE, chunk, &chunk_len);
            if (status == MODBUS_OK || status == MODBUS_END_OF_FILE)
                break;
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        if (download_stop_requested || download_generation != my_gen ||
            status != MODBUS_OK || chunk_len == 0) {
            send_browser_log("Failed first chunk or stopped before body");
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_sendstr(req, "Failed to read archive");
            download_in_progress = false;
            download_stop_requested = false;
            return ESP_OK;
        }

        if (chunk_len >= HEADER_SIZE) {
            extract_filename(chunk, &chunk_len);
            name_extracted = true;
        }

        httpd_resp_set_type(req, "application/octet-stream");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
        httpd_resp_set_hdr(req, "Pragma", "no-cache");
        httpd_resp_set_hdr(req, "Connection", "close");

        char encoded[900];
        char disp[1024];
        char xname[900];
        url_encode(archive_name, encoded, sizeof(encoded));
        snprintf(disp, sizeof(disp),
                 "attachment; filename=\"archive.bin\"; filename*=UTF-8''%s",
                 encoded);
        snprintf(xname, sizeof(xname), "%s", encoded);
        httpd_resp_set_hdr(req, "Content-Disposition", disp);
        httpd_resp_set_hdr(req, "X-File-Name", xname);

        if (chunk_len > 0) {
            if (!send_chunk_to_client(req, (const char *)chunk, chunk_len)) {
                aborted = true;
            } else {
                downloaded += chunk_len;
                chunk_counter = 1;
            }
        }
        offset = CHUNK_SIZE;
    }

    while (!aborted && !download_stop_requested &&
           download_generation == my_gen &&
           downloaded < total_size) {

        modbus_status_t status = MODBUS_ERR;
        for (int i = 0; i < MAX_RETRIES; i++) {
            if (download_stop_requested || download_generation != my_gen) break;
            status = modbus_read_file_0x64(offset, CHUNK_SIZE, chunk, &chunk_len);
            if (status == MODBUS_OK || status == MODBUS_END_OF_FILE)
                break;
            vTaskDelay(pdMS_TO_TICKS(30));
        }

        if (download_stop_requested || download_generation != my_gen) {
            aborted = true;
            break;
        }
        if (status == MODBUS_END_OF_FILE) {
            send_browser_log("End of file reached");
            break;
        }
        if (status != MODBUS_OK || chunk_len == 0) {
            send_browser_log("Too many errors, stopping");
            aborted = true;
            break;
        }

        if (!send_chunk_to_client(req, (const char *)chunk, chunk_len)) {
            aborted = true;
            break;
        }

        downloaded += chunk_len;
        offset += CHUNK_SIZE;
        chunk_counter++;

        if ((chunk_counter & 1) == 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }

    aborted = aborted || download_stop_requested || (download_generation != my_gen);
    (void)name_extracted;

    if (aborted) {
        send_browser_log("Download aborted - no final chunk");
        {
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "{\"type\":\"downloadStopped\",\"received\":%lu,\"fileName\":\"%s\"}",
                     (unsigned long)downloaded, archive_name);
            web_server_send(msg);
        }
        download_in_progress = false;
        download_stop_requested = false;
        return ESP_FAIL;
    }

    send_browser_log(downloaded >= total_size
                     ? "Sending final chunk - download complete"
                     : "Partial download, closing stream");
    (void)httpd_resp_send_chunk(req, NULL, 0);

    {
        char msg[512];
        if (downloaded >= total_size) {
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
    }

    download_in_progress = false;
    download_stop_requested = false;
    return ESP_OK;
}

void archive_manager_on_ws_command(const char *cmd)
{
    if (cmd == NULL) return;

    if (strstr(cmd, "\"getArchiveSize\"") || strstr(cmd, "\"getArchiveInfo\"") ||
        strstr(cmd, "\"action\":\"getArchiveSize\"")) {
        uint32_t size = 0;
        if (modbus_read_archive_size(&size)) {
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

    if (strstr(cmd, "\"startDownload\"") || strstr(cmd, "\"action\":\"startDownload\"")) {
        if (download_in_progress) {
            archive_manager_stop_download();
            web_server_send("{\"type\":\"log\",\"msg\":\"Previous download stop requested\"}");
        }
        web_server_send("{\"type\":\"log\",\"msg\":\"Download start requested\"}");
        return;
    }

    if (strstr(cmd, "\"stopDownload\"") || strstr(cmd, "\"action\":\"stopDownload\"")) {
        archive_manager_stop_download();
        web_server_send("{\"type\":\"log\",\"msg\":\"Download stopped by user\"}");
        return;
    }
}
