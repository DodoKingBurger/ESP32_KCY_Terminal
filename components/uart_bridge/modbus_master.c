#include "modbus_master.h"

#include "uart_bridge.h"
#include "mbcrc.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

/**
 * @brief Запись порции файла прошивки через FC 0x65.
 * Формат: addr | 0x65 | file_id(2 BE) | offset(4 BE) | size(4 BE) | data[N] | CRC16
 * Ответ:  addr | 0x65 | file_id(2) | offset(4) | received_size(4) | CRC16
 */
static char s_0x65_last_err[96] = "";

/*
====================================================
BUILD REQUEST
====================================================
*/
/**
 * @brief Формирует Modbus-запрос для чтения регистров (функции 0x03, 0x04 и др.)
 * @param function  код функции Modbus
 * @param reg       начальный адрес регистра
 * @param count     количество регистров
 * @param req       указатель на буфер для запроса (минимум 8 байт)
 */
static void build_request(
    uint8_t function,
    uint16_t reg,
    uint16_t count,
    uint8_t *req
)
{
    req[0] = SLAVE_ID;

    req[1] = function;

    req[2] = reg >> 8;
    req[3] = reg & 0xFF;

    req[4] = count >> 8;
    req[5] = count & 0xFF;

    mbcrc_insert_crc(req, 6);
}

/*
====================================================
EXECUTE REQUEST
====================================================
*/
/**
 * @brief Отправляет запрос, получает ответ и проверяет CRC
 * @param request      буфер с запросом
 * @param request_len  длина запроса
 * @param response     буфер для ответа
 * @param response_len указатель для сохранения длины ответа
 * @return MODBUS_OK при успехе, иначе код ошибки
 */
static modbus_status_t execute_request(
    uint8_t *request,
    uint16_t request_len,
    uint8_t *response,
    uint16_t *response_len
)
{
    int len =
        uart_bridge_transact(
            request,
            request_len,
            response,
            256,
            MODBUS_TIMEOUT_MS
        );

    if (len < 0)
    {
        return MODBUS_ERR_UART;
    }
    if (len <= 0)
    {
        return MODBUS_ERR_TIMEOUT;
    }

    /*
    Минимальный размер Modbus RTU:
    slave + func + bytes + crc
    */

    if (len < 5)
    {
        return MODBUS_ERR_UART;
    }

    /*
    Проверка CRC
    */

    if (!mbcrc_is_valid(response, len))
    {
        return MODBUS_ERR_CRC;
    }

    /*
    Проверка exception
    */

    if (response[1] & 0x80)
    {
        return MODBUS_ERR_UART;
    }

    *response_len = len;

    return MODBUS_OK;
}

/*
====================================================
READ HOLDING
====================================================
*/
/**
 * @brief Чтение holding-регистров (функция 0x03)
 */
modbus_status_t modbus_read_holding(
    uint16_t reg,
    uint16_t count,
    uint8_t *response,
    uint16_t *response_len
)
{
    uint8_t request[8];

    build_request(
        0x03,
        reg,
        count,
        request
    );

    return execute_request(
        request,
        sizeof(request),
        response,
        response_len
    );
}

/*
====================================================
READ INPUT
====================================================
*/
/**
 * @brief Чтение input-регистров (функция 0x04)
 */
modbus_status_t modbus_read_input(
    uint16_t reg,
    uint16_t count,
    uint8_t *response,
    uint16_t *response_len
)
{
    uint8_t request[8];

    build_request(
        0x04,
        reg,
        count,
        request
    );

    return execute_request(
        request,
        sizeof(request),
        response,
        response_len
    );
}

/*
====================================================
HELPERS
====================================================
*/

/**
 * @brief Чтение нескольких регистров с распаковкой в массив uint16_t
 * @param start_reg  начальный адрес
 * @param count      количество регистров
 * @param values     массив для сохранения значений (должен быть размером count)
 * @return MODBUS_OK при успехе
 */
modbus_status_t modbus_read_registers(
    uint16_t start_reg,
    uint16_t count,
    uint16_t *values
) {
    uint8_t request[8];
    build_request(0x04, start_reg, count, request); // 0x04 = read input registers

    uint8_t response[256];
    uint16_t resp_len = 0;
    modbus_status_t status = execute_request(request, sizeof(request), response, &resp_len);
    if (status != MODBUS_OK) return status;

    // Проверка количества байт: должно быть count * 2
    if (response[2] != count * 2) return MODBUS_ERR_UART;

    // Распаковка значений
    for (int i = 0; i < count; i++) {
        values[i] = (response[3 + i*2] << 8) | response[4 + i*2];
    }
    return MODBUS_OK;
}

/*
========================================================
READ SCREEN
Function 0x64
File 11
Offset 0
Size 2000
========================================================
*/
/**
 * @brief Чтение файла через функцию 0x64 (произвольное чтение)
 * @param offset   смещение в байтах
 * @param size     запрашиваемое количество байт (не более 4096)
 * @param out      буфер для данных
 * @param out_len  указатель для сохранения фактической длины
 * @return MODBUS_OK при успехе, при ошибке 0x03 (конец файла) возвращает MODBUS_OK с out_len=0
 */
modbus_status_t modbus_read_file_0x64(uint32_t offset, 
    uint32_t size, uint8_t *out, uint16_t *out_len)
{   
    if (size > 2048) {
        return MODBUS_ERR_UART;
    }
    if (size == 0) {
        *out_len = 0;
        return MODBUS_OK;
    }
    uint8_t request[32];
    int pos = 0;
    request[pos++] = SLAVE_ID;
    request[pos++] = 0x64;
    request[pos++] = 0x00;
    request[pos++] = 0x01;
    request[pos++] = (offset >> 24) & 0xFF;
    request[pos++] = (offset >> 16) & 0xFF;
    request[pos++] = (offset >> 8) & 0xFF;
    request[pos++] = offset & 0xFF;
    request[pos++] = (size >> 24) & 0xFF;
    request[pos++] = (size >> 16) & 0xFF;
    request[pos++] = (size >> 8) & 0xFF;
    request[pos++] = size & 0xFF;

    uint16_t crc = modbus_crc16(request, pos);
    request[pos++] = crc & 0xFF;
    request[pos++] = crc >> 8;

    uint8_t response[4096];
    int len = uart_bridge_transact(request, (size_t)pos, response, sizeof(response), 500);
    if (len < 0) {
        return MODBUS_ERR_UART;
    }

    if (len <= 0) {
        return MODBUS_ERR_TIMEOUT;
    }
    if (response[1] == 0xE4) {
        //if (err_code == 0x03) {
            //*out_len = 0;
            //return MODBUS_END_OF_FILE;
        //}
        return MODBUS_ERR_UART;
    }
    if (response[1] != 0x64) {
        return MODBUS_ERR_UART;
    }
    if (!mbcrc_is_valid(response, len)) {
        return MODBUS_ERR_CRC;
    }

    uint32_t resp_size = (response[8] << 24) | (response[9] << 16) | (response[10] << 8) | response[11];

    if (resp_size == 0) {
        *out_len = 0;
        return MODBUS_END_OF_FILE;
    }

        int data_offset = 12;
        int data_len = len - data_offset - 2;
    if (data_len <= 0) {
        return MODBUS_ERR_UART;
    }
    if (data_len > resp_size) data_len = resp_size;
    if (data_len > size) data_len = size;
    memcpy(out, &response[data_offset], data_len);
    *out_len = data_len;
        
    return MODBUS_OK;
}

const char *modbus_write_file_0x65_last_error(void)
{
    return s_0x65_last_err;
}

/*
========================================================
WRITE FIRMWARE FILE
Function 0x65
File id 0x0008 — файл прошивки оборудования СУ (ИРЗ)
Chunk ≤ 2000 bytes, offset/size big-endian
========================================================
*/
modbus_status_t modbus_write_file_0x65(uint16_t file_id,
    uint32_t offset, const uint8_t *data, uint32_t data_len,
    bool use_classic_uart)
{
    s_0x65_last_err[0] = '\0';

    if (data == NULL || data_len == 0) {
        snprintf(s_0x65_last_err, sizeof(s_0x65_last_err), "empty data");
        return MODBUS_ERR_UART;
    }
    if (data_len > 2000) {
        snprintf(s_0x65_last_err, sizeof(s_0x65_last_err), "chunk>2000");
        return MODBUS_ERR_UART;
    }

    /* Статический TX-буфер — не раздуваем стек httpd */
    static uint8_t request[2016];
    int pos = 0;
    request[pos++] = SLAVE_ID;
    request[pos++] = 0x65;
    request[pos++] = (file_id >> 8) & 0xFF;
    request[pos++] = file_id & 0xFF;
    request[pos++] = (offset >> 24) & 0xFF;
    request[pos++] = (offset >> 16) & 0xFF;
    request[pos++] = (offset >> 8) & 0xFF;
    request[pos++] = offset & 0xFF;
    request[pos++] = (data_len >> 24) & 0xFF;
    request[pos++] = (data_len >> 16) & 0xFF;
    request[pos++] = (data_len >> 8) & 0xFF;
    request[pos++] = data_len & 0xFF;
    memcpy(&request[pos], data, data_len);
    pos += (int)data_len;

    uint16_t crc = modbus_crc16(request, (uint16_t)pos);
    request[pos++] = crc & 0xFF;
    request[pos++] = (crc >> 8) & 0xFF;

    ESP_LOGI("MB", "0x65 TX offset=%lu data_len=%lu pkt=%d file=0x%04X classic=%d",
             (unsigned long)offset, (unsigned long)data_len, pos, (unsigned)file_id,
             (int)use_classic_uart);

    uint8_t response[64];
    /*
     * classic = тот же путь, что архив (0x64) — без post-TX flush.
     * long_tx  = сброс эха после длинного кадра (только 0x65).
     * Архив по-прежнему вызывает только uart_bridge_transact.
     */
    int len = use_classic_uart
        ? uart_bridge_transact(request, (size_t)pos, response, sizeof(response), 5000)
        : uart_bridge_transact_long_tx(request, (size_t)pos, response, sizeof(response), 5000);
    if (len < 0) {
        snprintf(s_0x65_last_err, sizeof(s_0x65_last_err), "uart write/transact fail");
        ESP_LOGW("MB", "0x65 %s offset=%lu", s_0x65_last_err, (unsigned long)offset);
        return MODBUS_ERR_UART;
    }
    if (len <= 0) {
        snprintf(s_0x65_last_err, sizeof(s_0x65_last_err), "TIMEOUT no response");
        ESP_LOGW("MB", "0x65 %s offset=%lu data_len=%lu classic=%d",
                 s_0x65_last_err, (unsigned long)offset, (unsigned long)data_len,
                 (int)use_classic_uart);
        return MODBUS_ERR_TIMEOUT;
    }

    /* Дамп начала ответа в лог — чтобы видеть exception / чужой кадр */
    ESP_LOGW("MB", "0x65 RX len=%d bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
             len,
             len > 0 ? response[0] : 0, len > 1 ? response[1] : 0,
             len > 2 ? response[2] : 0, len > 3 ? response[3] : 0,
             len > 4 ? response[4] : 0, len > 5 ? response[5] : 0,
             len > 6 ? response[6] : 0, len > 7 ? response[7] : 0);

    if (len >= 2 && response[1] == (uint8_t)(0x65 | 0x80)) {
        /* Стандартный exception: 5 байт. Без валидного CRC это скорее мусор/эхо. */
        if (len >= 5 && mbcrc_is_valid(response, 5)) {
            uint8_t exc = response[2];
            const char *hint = "vendor/IRZ";
            switch (exc) {
                case 0x01: hint = "illegal function — протокол не ИРЗ?"; break;
                case 0x02: hint = "illegal address"; break;
                case 0x03: hint = "illegal value"; break;
                case 0x04: hint = "device failure"; break;
                case 0x06: hint = "device busy"; break;
                case 0x21: hint = "IRZ reject (файл/заголовок/file id 0x0008)"; break;
                default: break;
            }
            snprintf(s_0x65_last_err, sizeof(s_0x65_last_err),
                     "exception 0x%02X (%s)", exc, hint);
            ESP_LOGW("MB", "0x65 %s offset=%lu", s_0x65_last_err, (unsigned long)offset);
            return MODBUS_ERR_UART;
        }
        snprintf(s_0x65_last_err, sizeof(s_0x65_last_err),
                 "noise RX len=%d [%02X %02X %02X %02X %02X %02X %02X] (echo?)",
                 len,
                 len > 0 ? response[0] : 0, len > 1 ? response[1] : 0,
                 len > 2 ? response[2] : 0, len > 3 ? response[3] : 0,
                 len > 4 ? response[4] : 0, len > 5 ? response[5] : 0,
                 len > 6 ? response[6] : 0);
        ESP_LOGW("MB", "0x65 %s offset=%lu", s_0x65_last_err, (unsigned long)offset);
        return MODBUS_ERR_CRC;
    }
    if (len < 2 || response[1] != 0x65) {
        snprintf(s_0x65_last_err, sizeof(s_0x65_last_err),
                 "bad FC=0x%02X rx_len=%d (not 0x65)",
                 len >= 2 ? response[1] : 0, len);
        ESP_LOGW("MB", "0x65 %s offset=%lu", s_0x65_last_err, (unsigned long)offset);
        return MODBUS_ERR_UART;
    }
    if (len < 14) {
        snprintf(s_0x65_last_err, sizeof(s_0x65_last_err), "short resp len=%d", len);
        ESP_LOGW("MB", "0x65 %s offset=%lu", s_0x65_last_err, (unsigned long)offset);
        return MODBUS_ERR_UART;
    }
    if (!mbcrc_is_valid(response, (uint16_t)len)) {
        snprintf(s_0x65_last_err, sizeof(s_0x65_last_err), "CRC fail rx_len=%d", len);
        ESP_LOGW("MB", "0x65 %s offset=%lu", s_0x65_last_err, (unsigned long)offset);
        return MODBUS_ERR_CRC;
    }

    uint32_t ack_size = ((uint32_t)response[8] << 24) | ((uint32_t)response[9] << 16) |
                        ((uint32_t)response[10] << 8) | (uint32_t)response[11];
    if (ack_size != data_len) {
        ESP_LOGW("MB", "0x65 size mismatch: sent=%lu ack=%lu",
                 (unsigned long)data_len, (unsigned long)ack_size);
    }
    s_0x65_last_err[0] = '\0';
    return MODBUS_OK;
}

/**
 * @brief Чтение файла через функцию 0x14 (Read File Record)
 * @param file_id        идентификатор файла (1..)
 * @param record_number  номер записи (0..)
 * @param out            буфер для данных (максимум 200 байт)
 * @param out_len        указатель для сохранения длины данных
 * @return MODBUS_OK при успехе, при конце файла возвращает MODBUS_OK с out_len=0
 */
modbus_status_t modbus_read_file_0x14(uint16_t file_id, uint16_t record_number, uint8_t *out, uint16_t *out_len)
{
    uint8_t request[16];
    int pos = 0;
    request[pos++] = SLAVE_ID;
    request[pos++] = 0x14;
    request[pos++] = 0x07;                     // размер запроса (без CRC)
    request[pos++] = 0x06;                     // тип ссылки
    request[pos++] = (file_id >> 8) & 0xFF;
    request[pos++] = file_id & 0xFF;
    request[pos++] = (record_number >> 8) & 0xFF;
    request[pos++] = record_number & 0xFF;
    request[pos++] = 0x00;
    request[pos++] = 0x64;                     // record_length = 100 регистров (200 байт)

    uint16_t crc = modbus_crc16(request, pos);
    request[pos++] = crc & 0xFF;
    request[pos++] = (crc >> 8) & 0xFF;

    uint8_t response[256];
    int len = uart_bridge_transact(request, (size_t)pos, response, sizeof(response), 3000);
    if (len < 0) {
        ESP_LOGE("MODBUS", "0x14 send failed");
        return MODBUS_ERR_UART;
    }
    if (len <= 0) {
        ESP_LOGE("MODBUS", "0x14 timeout");
        return MODBUS_ERR_TIMEOUT;
    }
    if (len < 6) {
        ESP_LOGE("MODBUS", "0x14 response too short");
        return MODBUS_ERR_UART;
    }
    if (!mbcrc_is_valid(response, len)) {
        ESP_LOGE("MODBUS", "0x14 CRC error");
        return MODBUS_ERR_CRC;
    }

    if (response[1] == 0x94) {
        uint8_t err = response[2];
        ESP_LOGW("MODBUS", "0x14 error code: 0x%02X", err);
        if (err == 0x03) {
            *out_len = 0;
            return MODBUS_OK;
        }
        return MODBUS_ERR_UART;
    }
    if (response[1] != 0x14) {
        ESP_LOGE("MODBUS", "0x14 wrong function: 0x%02X", response[1]);
        return MODBUS_ERR_UART;
    }

    if (response[2] == 0x02 && response[3] == 0x01 && response[4] == 0x06) {
        *out_len = 0;
        return MODBUS_END_OF_FILE;
    }

    if (response[4] != 0x06) {
        ESP_LOGE("MODBUS", "0x14 wrong ref type: 0x%02X", response[4]);
        return MODBUS_ERR_UART;
    }

    uint8_t data_len = response[3];
    if (data_len == 0) {
        *out_len = 0;
        return MODBUS_OK;
    }

    int data_start = 5;
    int available = len - data_start - 2; // минус CRC
    if (data_len > available) data_len = available;
    memcpy(out, response + data_start, data_len);
    *out_len = data_len;
    return MODBUS_OK;
}

/**
 * @brief Получение размера архива (регистры 0x8FA, 0x8FB)
 * @param size  указатель для сохранения размера в байтах
 * @return true при успехе
 */
bool modbus_read_archive_size(uint32_t *size) {
    uint16_t regs[2];
    modbus_status_t status = modbus_read_registers(0x08FA, 2, regs);
    if (status != MODBUS_OK) {
        ESP_LOGE("MODBUS", "Failed to read archive size: %d", status);
        return false;
    }
    *size = ((uint32_t)regs[0] << 16) | regs[1];
    return true;
}