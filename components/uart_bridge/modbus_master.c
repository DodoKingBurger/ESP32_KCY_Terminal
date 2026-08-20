#include "modbus_master.h"

#include "uart_bridge.h"
#include "mbcrc.h"
#include "esp_log.h"

#include <string.h>

#define MODBUS_TIMEOUT_MS 200
/*
====================================================
BUILD REQUEST
====================================================
*/
/**
 * @brief Формирует Modbus-запрос для чтения регистров (функции 0x03, 0x04 и др.)
 * @param slave     адрес ведомого
 * @param function  код функции Modbus
 * @param reg       начальный адрес регистра
 * @param count     количество регистров
 * @param req       указатель на буфер для запроса (минимум 8 байт)
 */
static void build_request(
    uint8_t slave,
    uint8_t function,
    uint16_t reg,
    uint16_t count,
    uint8_t *req
)
{
    req[0] = slave;

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
    uint8_t slave,
    uint16_t reg,
    uint16_t count,
    uint8_t *response,
    uint16_t *response_len
)
{
    uint8_t request[8];

    build_request(
        slave,
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
    uint8_t slave,
    uint16_t reg,
    uint16_t count,
    uint8_t *response,
    uint16_t *response_len
)
{
    uint8_t request[8];

    build_request(
        slave,
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
 * @param slave      адрес ведомого
 * @param start_reg  начальный адрес
 * @param count      количество регистров
 * @param values     массив для сохранения значений (должен быть размером count)
 * @return MODBUS_OK при успехе
 */
modbus_status_t modbus_read_registers(
    uint8_t slave,
    uint16_t start_reg,
    uint16_t count,
    uint16_t *values
) {
    uint8_t request[8];
    build_request(slave, 0x04, start_reg, count, request); // 0x04 = read input registers

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

/**
 * @brief Чтение holding-регистров (0x03) с распаковкой в uint16_t[]
 */
modbus_status_t modbus_read_holding_registers(
    uint8_t slave,
    uint16_t start_reg,
    uint16_t count,
    uint16_t *values
) {
    uint8_t request[8];
    build_request(slave, 0x03, start_reg, count, request);

    uint8_t response[256];
    uint16_t resp_len = 0;
    modbus_status_t status = execute_request(request, sizeof(request), response, &resp_len);
    if (status != MODBUS_OK) return status;

    if (resp_len < 3 || response[2] != count * 2) return MODBUS_ERR_UART;

    for (int i = 0; i < count; i++) {
        values[i] = (response[3 + i*2] << 8) | response[4 + i*2];
    }
    return MODBUS_OK;
}

/**
 * @brief Запись одного регистра (функция 0x06)
 */
modbus_status_t modbus_write_single_register(
    uint8_t slave,
    uint16_t reg,
    uint16_t value
)
{
    uint8_t req[8];

    req[0] = slave;
    req[1] = 0x06;

    req[2] = reg >> 8;
    req[3] = reg & 0xFF;

    req[4] = value >> 8;
    req[5] = value & 0xFF;

    mbcrc_insert_crc(req, 6);

    uint8_t resp[32];
    uint16_t resp_len;

    return execute_request(
        req,
        8,
        resp,
        &resp_len
    );
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
 * @param slave    адрес ведомого
 * @param offset   смещение в байтах
 * @param size     запрашиваемое количество байт (не более 4096)
 * @param out      буфер для данных
 * @param out_len  указатель для сохранения фактической длины
 * @return MODBUS_OK при успехе, при ошибке 0x03 (конец файла) возвращает MODBUS_OK с out_len=0
 */
/**
 * @brief Чтение файла через функцию 0x64 (произвольное чтение)
 * @param slave    адрес ведомого
 * @param offset   смещение в байтах
 * @param size     запрашиваемое количество байт (не более 4096)
 * @param out      буфер для данных
 * @param out_len  указатель для сохранения фактической длины
 * @return MODBUS_OK при успехе, при ошибке 0x03 (конец файла) возвращает MODBUS_OK с out_len=0
 */
modbus_status_t modbus_read_file_0x64(uint8_t slave, uint32_t offset, 
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
    request[pos++] = slave;
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

/**
 * @brief Чтение файла через функцию 0x14 (Read File Record)
 * @param slave          адрес ведомого
 * @param file_id        идентификатор файла (1..)
 * @param record_number  номер записи (0..)
 * @param out            буфер для данных (максимум 200 байт)
 * @param out_len        указатель для сохранения длины данных
 * @return MODBUS_OK при успехе, при конце файла возвращает MODBUS_OK с out_len=0
 */
modbus_status_t modbus_read_file_0x14(uint8_t slave, uint16_t file_id, uint16_t record_number, uint8_t *out, uint16_t *out_len)
{
    // Запрос всегда на 100 регистров (200 байт) – фиксировано
    uint8_t request[16];
    int pos = 0;
    request[pos++] = slave;
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

    // Проверка на ошибку (код функции с битом 0x80)
    if (response[1] == 0x94) {
        uint8_t err = response[2];
        ESP_LOGW("MODBUS", "0x14 error code: 0x%02X", err);
        if (err == 0x03) {
            // Ошибка 0x03 – обычно конец файла
            *out_len = 0;
            return MODBUS_OK;
        }
        return MODBUS_ERR_UART;
    }
    if (response[1] != 0x14) {
        ESP_LOGE("MODBUS", "0x14 wrong function: 0x%02X", response[1]);
        return MODBUS_ERR_UART;
    }

    // Признак конца файла: byte_count=0x02, data_len=0x01, ref_type=0x06 (без данных)
    if (response[2] == 0x02 && response[3] == 0x01 && response[4] == 0x06) {
        *out_len = 0;
        return MODBUS_END_OF_FILE;
    }

    // Проверка типа ссылки
    if (response[4] != 0x06) {
        ESP_LOGE("MODBUS", "0x14 wrong ref type: 0x%02X", response[4]);
        return MODBUS_ERR_UART;
    }

    // data_len = response[3] (количество байт данных)
    uint8_t data_len = response[3];
    if (data_len == 0) {
        *out_len = 0;
        return MODBUS_OK;
    }

    // Данные начинаются с индекса 5 (после ref_type)
    int data_start = 5;
    int available = len - data_start - 2; // минус CRC
    if (data_len > available) data_len = available;
    memcpy(out, response + data_start, data_len);
    *out_len = data_len;
    return MODBUS_OK;
}

/**
 * @brief Получение размера архива (регистры 0x8FA, 0x8FB)
 * @param slave адрес ведомого
 * @param size  указатель для сохранения размера в байтах
 * @return true при успехе
 */
bool modbus_read_archive_size(uint8_t slave, uint32_t *size) {
    uint16_t regs[2];
    modbus_status_t status = modbus_read_registers(slave, 0x08FA, 2, regs);
    if (status != MODBUS_OK) {
        ESP_LOGE("MODBUS", "Failed to read archive size: %d", status);
        return false;
    }
    *size = ((uint32_t)regs[0] << 16) | regs[1];
    return true;
}