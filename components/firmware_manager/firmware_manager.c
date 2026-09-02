
#include "firmware_manager.h"
#include "esp_ota_update.h"
#include "web_server.h"
#include "modbus_master.h"
#include "uart_bridge.h"
#include "mbcrc.h"

#include "esp_log.h"
#include "esp_http_server.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TAG "FW"

/* ИРЗ рекомендует 2000; эхо обрабатывается в uart_bridge_transact_long_tx */
#define FW_CHUNK           2000
#define FW_FILE_ID         0x0008
#define VERSION_MAX        64

static SemaphoreHandle_t fw_mutex = NULL;
static bool fw_busy = false;
static char cached_version[VERSION_MAX] = "—";

extern bool load_page_active;

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
static bool read_holding_u16(uint16_t reg, uint16_t *value)
{
    uint8_t resp[64];
    uint16_t resp_len = 0;
    modbus_status_t st = modbus_read_holding(reg, 1, resp, &resp_len);
    if (st != MODBUS_OK) {
        ESP_LOGW(TAG, "modbus_read_holding 0x%04X failed: %d", (unsigned)reg, (int)st);
        return false;
    }
    if (resp_len < 5 || resp[1] != 0x03 || resp[2] < 2) {
        ESP_LOGW(TAG, "bad holding response reg=0x%04X len=%u", (unsigned)reg, (unsigned)resp_len);
        return false;
    }
    *value = (uint16_t)((resp[3] << 8) | resp[4]);
    return true;
}

/**
 * Версия прошивки: holding 0x430 / 0x431 → "XX.YY.VVVV"
 */
bool firmware_manager_read_version(char *out, size_t out_len)
{
    if (!out || out_len == 0) return false;

    uint16_t reg430 = 0;
    uint16_t reg431 = 0;

    if (!read_holding_u16(0x430, &reg430) ||
        !read_holding_u16(0x431, &reg431)) {
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
 * Запись порции файла прошивки по UART: FC 0x65, file_id 0x0008.
 * offset — смещение от начала файла (включая 16-байтный заголовок).
 * Файл уже должен быть упакован: 16 байт header + body + FFFFFFFF.
 */
static char s_fw_fail_detail[128] = "";

static const char *fw_st_str(modbus_status_t st)
{
    switch (st) {
        case MODBUS_OK: return "OK";
        case MODBUS_ERR_TIMEOUT: return "TIMEOUT (нет ответа КСУ)";
        case MODBUS_ERR_CRC: return "CRC";
        case MODBUS_ERR_UART: return "UART/exception";
        default: return "ERR";
    }
}

static bool firmware_write_chunk_uart(const uint8_t *data, size_t len, uint32_t offset,
                                     modbus_status_t *out_st)
{
    s_fw_fail_detail[0] = '\0';
    if (!data || len == 0) {
        if (out_st) *out_st = MODBUS_ERR_UART;
        snprintf(s_fw_fail_detail, sizeof(s_fw_fail_detail), "empty chunk");
        return false;
    }

    /*
     * По логу: long_tx → TIMEOUT, classic → OK.
     * Шлём только classic (как архив 0x64). При OK — сразу return, без
     * оставшихся попыток. Повтор только при реальном сбое (до MAX_RETRIES).
     */
    modbus_status_t last = MODBUS_ERR;
    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        last = modbus_write_file_0x65(
            FW_FILE_ID, offset, data, (uint32_t)len, true /* classic */);

        if (last == MODBUS_OK) {
            if (out_st) *out_st = last;
            return true;
        }

        const char *det = modbus_write_file_0x65_last_error();
        snprintf(s_fw_fail_detail, sizeof(s_fw_fail_detail), "%s",
                 (det && det[0]) ? det : fw_st_str(last));

        char failm[160];
        snprintf(failm, sizeof(failm),
                 "0x65 fail try=%d/%d off=%lu st=%d (%s)",
                 attempt + 1, MAX_RETRIES, (unsigned long)offset,
                 (int)last, s_fw_fail_detail);
        send_log(failm);
        ESP_LOGW(TAG, "%s", failm);

        if (attempt + 1 < MAX_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(50 + attempt * 50));
        }
    }
    if (out_st) *out_st = last;
    return false;
}

/**
 * FC 0x06: reg 0x0000.
 * Свой transact + таймаут 3 с (execute_request даёт 200 мс → st=-3 после длинной 0x65).
 * TX/RX в консоль браузера.
 */
bool firmware_manager_send_reflash_command(void)
{
    /* 0x20 = КСУ Linux (hex), в пакете value = 00 20 */
    uint16_t code = FW_DEVICE_CODE_KSULINUX;

    uint8_t req[8];
    req[0] = (uint8_t)SLAVE_ID;
    req[1] = 0x06;
    req[2] = 0x00;
    req[3] = 0x00;
    req[4] = (uint8_t)(code >> 8);
    req[5] = (uint8_t)(code & 0xFF);
    mbcrc_insert_crc(req, 6);

    char txmsg[160];
    snprintf(txmsg, sizeof(txmsg),
             "FC06 TX: %02X %02X %02X %02X %02X %02X %02X %02X (reg=0x%04X val=0x%02X/%u)",
             req[0], req[1], req[2], req[3], req[4], req[5], req[6], req[7],
             0x00, (unsigned)code, (unsigned)code);
    send_log(txmsg);
    {
        char js[256];
        snprintf(js, sizeof(js), "{\"type\":\"firmwareDebug\",\"msg\":\"%s\"}", txmsg);
        web_server_send(js);
    }
    ESP_LOGI(TAG, "%s", txmsg);

    uint8_t resp[32];
    int len = uart_bridge_transact(req, sizeof(req), resp, sizeof(resp), 3000);

    char rxmsg[160];
    if (len <= 0) {
        snprintf(rxmsg, sizeof(rxmsg),
                 "FC06 RX: %s (len=%d)",
                 len < 0 ? "UART write fail" : "TIMEOUT", len);
        send_log(rxmsg);
        web_server_send("{\"type\":\"reflashStatus\",\"code\":-1,\"msg\":\"FC06 нет ответа (TIMEOUT/UART)\"}");
        {
            char js[200];
            snprintf(js, sizeof(js), "{\"type\":\"firmwareDebug\",\"msg\":\"%s\"}", rxmsg);
            web_server_send(js);
        }
        ESP_LOGW(TAG, "%s", rxmsg);
        return false;
    }

    {
        int n = len > 8 ? 8 : len;
        char hex[48] = {0};
        int hp = 0;
        for (int i = 0; i < n && hp < (int)sizeof(hex) - 4; i++) {
            hp += snprintf(hex + hp, sizeof(hex) - (size_t)hp, "%02X ", resp[i]);
        }
        snprintf(rxmsg, sizeof(rxmsg), "FC06 RX len=%d: %s", len, hex);
        send_log(rxmsg);
        {
            char js[200];
            snprintf(js, sizeof(js), "{\"type\":\"firmwareDebug\",\"msg\":\"%s\"}", rxmsg);
            web_server_send(js);
        }
        ESP_LOGI(TAG, "%s", rxmsg);
    }

    if (len < 5) {
        web_server_send("{\"type\":\"reflashStatus\",\"code\":-1,\"msg\":\"FC06 короткий ответ\"}");
        return false;
    }
    if (!mbcrc_is_valid(resp, (uint16_t)len)) {
        send_log("FC06 RX CRC fail");
        web_server_send("{\"type\":\"reflashStatus\",\"code\":-1,\"msg\":\"FC06 CRC fail\"}");
        return false;
    }
    if (resp[1] & 0x80) {
        uint8_t exc = (len >= 3) ? resp[2] : 0;
        char em[96];
        snprintf(em, sizeof(em), "FC06 exception 0x%02X", exc);
        send_log(em);
        {
            char js[140];
            snprintf(js, sizeof(js),
                     "{\"type\":\"reflashStatus\",\"code\":-1,\"msg\":\"%s\"}", em);
            web_server_send(js);
        }
        return false;
    }
    if (resp[1] != 0x06) {
        char em[80];
        snprintf(em, sizeof(em), "FC06 bad FC=0x%02X", resp[1]);
        send_log(em);
        web_server_send("{\"type\":\"reflashStatus\",\"code\":-1,\"msg\":\"FC06 bad function\"}");
        return false;
    }

    send_log("FC06 OK, deviceCode=0x20 (32)");
    return true;
}

/**
 * Читает 0x008A и шлёт reflashStatus в UI (поле «Статус»).
 * @return код статуса, -1 при ошибке чтения
 */
static int publish_reflash_status(void)
{
    uint16_t st = 0xFFFF;
    uint8_t resp[64];
    uint16_t resp_len = 0;
    modbus_status_t ms = modbus_read_input(REFLASH_STATUS_REG, 1, resp, &resp_len);
    if (ms == MODBUS_OK && resp_len >= 5 && resp[2] >= 2) {
        st = (uint16_t)((resp[3] << 8) | resp[4]);
    } else if (read_holding_u16(REFLASH_STATUS_REG, &st)) {
    } else {
        web_server_send("{\"type\":\"reflashStatus\",\"code\":-1,\"msg\":\"не удалось прочитать 0x008A\"}");
        return -1;
    }

    const char *desc = "неизвестно";
    switch (st) {
        case 0: desc = "прошивка завершена корректно"; break;
        case 1: desc = "ошибка CRC файла"; break;
        case 2: desc = "некорректный файл / тип устройства"; break;
        case 3: desc = "нет связи с внешним устройством"; break;
        case 6: desc = "ошибка программирования"; break;
        case 7: desc = "идёт процесс прошивки"; break;
        case 8: desc = "ошибка совместимости (изготовитель)"; break;
        case 9: desc = "ошибка файловой системы"; break;
        default: break;
    }

    char msg[200];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"reflashStatus\",\"code\":%u,\"msg\":\"%s\"}",
             (unsigned)st, desc);
    web_server_send(msg);
    return (int)st;
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

/**
 * После FC06: опрос 0x008A, пока код == 7 («идёт процесс»);
 * любой другой код → статус + обновить «Версия прошивки».
 */
static void poll_reflash_status_smart(void)
{
    int code = publish_reflash_status();
    int n = 0;
    while (code == 7 && n < 120) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        code = publish_reflash_status();
        n++;
    }

    vTaskDelay(pdMS_TO_TICKS(300));
    send_log("Reflash finished, updating firmware version");
    publish_version();
}

/* ===== HTTP POST /firmware =====
 * Тело = уже упакованный файл (16 байт header + данные + FFFFFFFF).
 * ESP режет на порции ≤2000 и шлёт FC 0x65 с offset.
 */
esp_err_t firmware_manager_http_upload_handler(httpd_req_t *req)
{
    if (fw_busy || esp_ota_update_is_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_sendstr(req, "Firmware transfer busy");
        return ESP_OK;
    }

    fw_busy = true;
    /* Пауза только кадров терминала; архив (download_in_progress) не трогаем */
    const bool prev_load_page = load_page_active;
    load_page_active = true;
    vTaskDelay(pdMS_TO_TICKS(250));

    send_log("Firmware upload started (FC 0x65)");

    int total = req->content_len;
    if (total <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_sendstr(req, "Empty body");
        fw_busy = false;
        load_page_active = prev_load_page;
        return ESP_OK;
    }

    {
        char msg[120];
        snprintf(msg, sizeof(msg),
                 "{\"type\":\"firmwareUploadStart\",\"size\":%d}", total);
        web_server_send(msg);
    }

    /*
     * Копим HTTP в порции ровно FW_CHUNK (2000), как рекомендует ИРЗ.
     * TCP часто отдаёт <2000 — раньше слали короткие 0x65 с offset 0.
     */
    static uint8_t chunk[FW_CHUNK];
    static uint8_t rx[FW_CHUNK];
    size_t chunk_fill = 0;
    int received = 0;
    uint32_t offset = 0;
    bool ok = true;
    modbus_status_t fail_st = MODBUS_OK;
    uint32_t fail_off = 0;

    while (received < total) {
        int to_read = total - received;
        if (to_read > (int)sizeof(rx)) to_read = (int)sizeof(rx);

        int r = httpd_req_recv(req, (char *)rx, to_read);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            send_log("Firmware upload aborted by client");
            ok = false;
            fail_st = MODBUS_ERR;
            fail_off = offset;
            snprintf(s_fw_fail_detail, sizeof(s_fw_fail_detail), "HTTP aborted");
            break;
        }

        int rx_pos = 0;
        while (rx_pos < r) {
            size_t space = FW_CHUNK - chunk_fill;
            size_t take = (size_t)(r - rx_pos);
            if (take > space) take = space;
            memcpy(chunk + chunk_fill, rx + rx_pos, take);
            chunk_fill += take;
            rx_pos += (int)take;
            received += (int)take;

            bool is_last = (received >= total);
            if (chunk_fill >= FW_CHUNK || (is_last && chunk_fill > 0)) {
                /* На offset 0 — разбор 16-байтного заголовка ИРЗ (LE) и лог в браузер */
                if (offset == 0 && chunk_fill >= 16) {
                    uint32_t fsz = (uint32_t)chunk[0] | ((uint32_t)chunk[1] << 8) |
                                   ((uint32_t)chunk[2] << 16) | ((uint32_t)chunk[3] << 24);
                    uint16_t mfg = (uint16_t)chunk[4] | ((uint16_t)chunk[5] << 8);
                    uint8_t dtype = chunk[6];
                    uint16_t rev = (uint16_t)chunk[8] | ((uint16_t)chunk[9] << 8);
                    char hlog[256];
                    snprintf(hlog, sizeof(hlog),
                             "FW hdr: size=%lu mfg=%u type=%u rev=%u (expect mfg=54)",
                             (unsigned long)fsz, (unsigned)mfg, (unsigned)dtype, (unsigned)rev);
                    send_log(hlog);
                    if (mfg != 54) {
                        snprintf(s_fw_fail_detail, sizeof(s_fw_fail_detail),
                                 "bad header mfg=%u (need 54 IRZ TEK)", (unsigned)mfg);
                        fail_st = MODBUS_ERR_UART;
                        fail_off = 0;
                        send_log(s_fw_fail_detail);
                        ok = false;
                        break;
                    }
                }

                modbus_status_t st = MODBUS_OK;
                if (!firmware_write_chunk_uart(chunk, chunk_fill, offset, &st)) {
                    fail_st = st;
                    fail_off = offset;
                    char em[256];
                    snprintf(em, sizeof(em),
                             "UART 0x65 failed at offset %lu st=%d (%s)",
                             (unsigned long)offset, (int)st,
                             s_fw_fail_detail[0] ? s_fw_fail_detail : fw_st_str(st));
                    send_log(em);
                    ok = false;
                    break;
                }
                offset += (uint32_t)chunk_fill;
                chunk_fill = 0;

                {
                    char prog[160];
                    float pct = total > 0 ? (100.0f * received / total) : 0;
                    snprintf(prog, sizeof(prog),
                             "{\"type\":\"firmwareProgress\",\"received\":%d,\"total\":%d,\"percent\":%.1f}",
                             received, total, pct);
                    web_server_send(prog);
                }
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }
        if (!ok) break;
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

        /*
         * После 0x65 → FC 0x06 (0x20) → пинг → опрос 0x008A до кода ≠7 → версия.
         */
        vTaskDelay(pdMS_TO_TICKS(800));
        send_log("Auto-reflash: FC 0x06 value=0x20 (32 dec)");
        if (firmware_manager_send_reflash_command()) {
            web_server_send("{\"type\":\"reflashStarted\",\"code\":32}");
            vTaskDelay(pdMS_TO_TICKS(500));
            poll_reflash_status_smart();
        } else {
            web_server_send(
                "{\"type\":\"error\",\"msg\":\"0x65 OK, but FC06 reflash failed\"}");
            send_log("Auto-reflash FC06 failed");
        }
    } else {
        /* msg без кавычек/слэшей — чтобы JSON не ломался */
        char safe[96];
        const char *src = s_fw_fail_detail[0] ? s_fw_fail_detail : fw_st_str(fail_st);
        size_t j = 0;
        for (size_t i = 0; src[i] && j + 1 < sizeof(safe); i++) {
            char c = src[i];
            if (c == '"' || c == '\\' || c < 32) c = ' ';
            safe[j++] = c;
        }
        safe[j] = '\0';

        char body[256];
        snprintf(body, sizeof(body),
                 "{\"ok\":false,\"offset\":%lu,\"st\":%d,\"msg\":\"%s\"}",
                 (unsigned long)fail_off, (int)fail_st, safe);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, body);

        char wmsg[280];
        snprintf(wmsg, sizeof(wmsg),
                 "{\"type\":\"firmwareUploadError\",\"offset\":%lu,\"st\":%d,\"msg\":\"%s\"}",
                 (unsigned long)fail_off, (int)fail_st, safe);
        web_server_send(wmsg);
    }

    fw_busy = false;
    /* Не оставляем терминал «заглушенным» навсегда после ошибки 0x65 */
    load_page_active = prev_load_page;
    return ESP_OK;
}

void firmware_manager_on_ws_command(const char *cmd)
{
    if (cmd == NULL) return;

    if (strstr(cmd, "getFirmwareVersion") || strstr(cmd, "\"action\":\"getFirmwareVersion\"")) {
        publish_version();
        return;
    }

    if (strstr(cmd, "getReflashStatus") || strstr(cmd, "\"action\":\"getReflashStatus\"")) {
        publish_reflash_status();
        return;
    }

    if (strstr(cmd, "startReflash") || strstr(cmd, "\"action\":\"startReflash\"")) {
        if (fw_busy) {
            web_server_send("{\"type\":\"error\",\"msg\":\"Firmware busy\"}");
            return;
        }
        bool ok = firmware_manager_send_reflash_command();
        if (ok) {
            web_server_send("{\"type\":\"reflashStarted\",\"code\":32}");
            vTaskDelay(pdMS_TO_TICKS(500));
            poll_reflash_status_smart();
        } else {
            web_server_send("{\"type\":\"error\",\"msg\":\"Reflash command failed\"}");
        }
        return;
    }
}
