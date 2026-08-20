
#include "firmware_manager.h"
#include "web_server.h"
#include "modbus_master.h"

#include "esp_log.h"
#include "esp_http_server.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TAG "FW"
#define FW_CHUNK 1024
#define VERSION_MAX 64

static SemaphoreHandle_t fw_mutex = NULL;
static bool fw_busy = false;
static char cached_version[VERSION_MAX] = "—";

static void send_log(const char *msg)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"type\":\"log\",\"msg\":\"%s\"}", msg);
    web_server_send(buf);
}

void firmware_manager_init(void)
{
    if (fw_mutex == NULL) {
        fw_mutex = xSemaphoreCreateMutex();
    }
}

bool firmware_manager_is_busy(void)
{
    return fw_busy;
}

/**
 * Ответ FC 0x03 на 1 регистр:
 *   [0]=slave [1]=0x03 [2]=byte_count(=2) [3]=hi [4]=lo [5..6]=CRC
 */
static bool read_holding_u16(uint8_t slave, uint16_t reg, uint16_t *value)
{
    uint8_t resp[64];
    uint16_t resp_len = 0;
    modbus_status_t st = modbus_read_holding(slave, reg, 1, resp, &resp_len);
    if (st != MODBUS_OK) {
        ESP_LOGW(TAG, "modbus_read_holding 0x%04X failed: %d", (unsigned)reg, (int)st);
        return false;
    }
    /* Минимум: addr+func+bc+2 data (+ CRC обычно уже проверен в execute_request) */
    if (resp_len < 5 || resp[1] != 0x03 || resp[2] < 2) {
        ESP_LOGW(TAG, "bad holding response reg=0x%04X len=%u", (unsigned)reg, (unsigned)resp_len);
        return false;
    }
    *value = (uint16_t)((resp[3] << 8) | resp[4]);
    return true;
}

/**
 * @brief Версия прошивки по UART (holding 0x430 и 0x431, FC 0x03, slave 1).
 *
 * Как в C# GetCurrentFirmwareVersion:
 *   ReadHoldingRegisters(0x430, 1) → xx = HI, yy = LO
 *   ReadHoldingRegisters(0x431, 1) → vvvv = значение регистра
 *   "XX.YY.VVVV"
 */
bool firmware_manager_read_version(char *out, size_t out_len)
{
    if (!out || out_len == 0) return false;

    uint16_t reg430 = 0;
    uint16_t reg431 = 0;

    if (!read_holding_u16(1, 0x430, &reg430)) {
        if (fw_mutex) xSemaphoreTake(fw_mutex, portMAX_DELAY);
        strncpy(out, cached_version, out_len - 1);
        out[out_len - 1] = '\0';
        if (fw_mutex) xSemaphoreGive(fw_mutex);
        return false;
    }

    if (!read_holding_u16(1, 0x431, &reg431)) {
        if (fw_mutex) xSemaphoreTake(fw_mutex, portMAX_DELAY);
        strncpy(out, cached_version, out_len - 1);
        out[out_len - 1] = '\0';
        if (fw_mutex) xSemaphoreGive(fw_mutex);
        return false;
    }

    const int xx = (reg430 >> 8) & 0xFF;
    const int yy = reg430 & 0xFF;
    const int vvvv = (int)reg431;

    char ver[VERSION_MAX];
    snprintf(ver, sizeof(ver), "%02d.%02d.%04d", xx, yy, vvvv);

    if (fw_mutex) xSemaphoreTake(fw_mutex, portMAX_DELAY);
    strncpy(cached_version, ver, VERSION_MAX - 1);
    cached_version[VERSION_MAX - 1] = '\0';
    strncpy(out, cached_version, out_len - 1);
    out[out_len - 1] = '\0';
    if (fw_mutex) xSemaphoreGive(fw_mutex);

    ESP_LOGI(TAG, "FW version: %s (0x430=0x%04X 0x431=0x%04X)", ver,
             (unsigned)reg430, (unsigned)reg431);
    return true;
}

/**
 * @brief Запись куска файла прошивки в устройство по UART.
 * TODO: реализовать запись (Modbus file write / XMODEM / свой протокол).
 */
static bool firmware_write_chunk_uart(const uint8_t *data, size_t len,
                                     uint32_t offset, uint32_t total)
{
    (void)data;
    (void)len;
    (void)offset;
    (void)total;
    /* Заглушка: считаем OK, чтобы UI-цепочка работала end-to-end */
    return true;
}

/**
 * @brief Команда «начать перепрошивку» по UART.
 * TODO: подставить реальную команду устройства.
 */
bool firmware_manager_send_reflash_command(void)
{
    /*
     * Пример:
     *   return terminal_send_command(1, "...") или modbus_write_single_register(...);
     */
    send_log("Reflash command sent (stub)");
    ESP_LOGW(TAG, "reflash command is STUB — implement UART protocol");
    return true;
}

static void publish_version(void)
{
    char ver[VERSION_MAX];
    if (!firmware_manager_read_version(ver, sizeof(ver))) {
        strncpy(ver, "ошибка чтения", sizeof(ver) - 1);
    }
    char msg[160];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"firmwareVersion\",\"version\":\"%s\"}", ver);
    web_server_send(msg);
}

/* ===== HTTP POST /firmware ===== */
esp_err_t firmware_manager_http_upload_handler(httpd_req_t *req)
{
    if (fw_busy) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_sendstr(req, "Firmware transfer busy");
        return ESP_OK;
    }

    fw_busy = true;
    send_log("Firmware upload started");

    int total = req->content_len;
    if (total <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_sendstr(req, "Empty body");
        fw_busy = false;
        return ESP_OK;
    }

    {
        char msg[120];
        snprintf(msg, sizeof(msg),
                 "{\"type\":\"firmwareUploadStart\",\"size\":%d}", total);
        web_server_send(msg);
    }

    uint8_t buf[FW_CHUNK];
    int received = 0;
    bool ok = true;

    while (received < total) {
        int to_read = total - received;
        if (to_read > (int)sizeof(buf)) to_read = (int)sizeof(buf);

        int r = httpd_req_recv(req, (char *)buf, to_read);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            send_log("Firmware upload aborted by client");
            ok = false;
            break;
        }

        if (!firmware_write_chunk_uart(buf, (size_t)r, (uint32_t)received, (uint32_t)total)) {
            send_log("UART write failed");
            ok = false;
            break;
        }

        received += r;

        /* прогресс каждые ~4 КБ */
        if ((received & 0xFFF) == 0 || received >= total) {
            char prog[160];
            float pct = total > 0 ? (100.0f * received / total) : 0;
            snprintf(prog, sizeof(prog),
                     "{\"type\":\"firmwareProgress\",\"received\":%d,\"total\":%d,\"percent\":%.1f}",
                     received, total, pct);
            web_server_send(prog);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    httpd_resp_set_hdr(req, "Connection", "close");
    if (ok && received >= total) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true}");
        {
            char msg[120];
            snprintf(msg, sizeof(msg),
                     "{\"type\":\"firmwareUploadComplete\",\"size\":%d}", received);
            web_server_send(msg);
        }
        send_log("Firmware upload complete");
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Upload failed");
        web_server_send("{\"type\":\"firmwareUploadError\",\"msg\":\"upload failed\"}");
    }

    fw_busy = false;
    return ESP_OK;
}

void firmware_manager_on_ws_command(const char *cmd)
{
    if (cmd == NULL) return;

    if (strstr(cmd, "getFirmwareVersion") || strstr(cmd, "\"action\":\"getFirmwareVersion\"")) {
        publish_version();
        return;
    }

    if (strstr(cmd, "startReflash") || strstr(cmd, "\"action\":\"startReflash\"")) {
        if (fw_busy) {
            web_server_send("{\"type\":\"error\",\"msg\":\"Firmware busy\"}");
            return;
        }
        bool ok = firmware_manager_send_reflash_command();
        if (ok) {
            web_server_send("{\"type\":\"reflashStarted\"}");
        } else {
            web_server_send("{\"type\":\"error\",\"msg\":\"Reflash command failed\"}");
        }
        return;
    }
}
