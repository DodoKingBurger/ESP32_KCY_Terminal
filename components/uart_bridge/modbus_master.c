#include "modbus_master.h"

#include "uart_bridge.h"
#include "mbcrc.h"

#include "esp_log.h"

#include <string.h>

#define MODBUS_TIMEOUT_MS 200
static const char *TAG = "MODBUS_POLL";
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
    int written =
        uart_bridge_send(
            request,
            request_len
        );

    if (written != request_len)
    {
        return MODBUS_ERR_UART;
    }

    int len =
        uart_bridge_receive(
            response,
            256,
            MODBUS_TIMEOUT_MS
        );

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
 * @brief Чтение экрана терминала через функцию 0x64 (файл 11, размер 2000)
 * @param slave_id   адрес ведомого
 * @param screen     буфер для принятых данных (ANSI-текст)
 * @param screen_len указатель для сохранения длины полученных данных
 * @return true при успехе
 */
bool terminal_read_screen(
    uint8_t slave_id,
    uint8_t *screen,
    uint16_t *screen_len
)
{
    uint8_t request[32];

    int pos = 0;

    request[pos++] = slave_id;
    request[pos++] = 0x64;

    /*
    FILE NUMBER = 11
    */

    request[pos++] = 0x00;
    request[pos++] = 0x0B;

    /*
    OFFSET = 0
    */

    request[pos++] = 0x00;
    request[pos++] = 0x00;
    request[pos++] = 0x00;
    request[pos++] = 0x00;

    /*
    SIZE = 2000 bytes
    */

    request[pos++] = 0x00;
    request[pos++] = 0x00;
    request[pos++] = 0x07;
    request[pos++] = 0xD0;

    /*
    CRC
    */

    uint16_t crc =
        modbus_crc16(request, pos);

    request[pos++] = crc & 0xFF;
    request[pos++] = crc >> 8;

    /*
    SEND REQUEST
    */

    uart_bridge_send(
        request,
        pos
    );

    /*
    RECEIVE RESPONSE
    */

    uint8_t response[4096];

    int len =
        uart_bridge_receive(
            response,
            sizeof(response),
            pdMS_TO_TICKS(1000)
        );

    if (len <= 0)
    {
        ESP_LOGW(TAG, "No response");

        return false;
    }

    /*
    MINIMAL CHECK
    */

    if (false && response[1] != 0x64)
    {
        ESP_LOGW(TAG, "Invalid function");

        return false;
    }

    /*
    DATA STARTS AFTER:
    slave + func + file + offset + size

    1 + 1 + 2 + 4 + 4 = 12
    */

    int data_offset = 12;

    /*
    CRC = last 2 bytes
    */

    int data_len =
        len - data_offset - 2;

    if (data_len <= 0)
    {
        ESP_LOGW(TAG, "Invalid data len");

        return false;
    }

    memcpy(
        screen,
        &response[data_offset],
        data_len
    );

    *screen_len = data_len;

    return true;
}

/*
========================================================
SEND COMMAND
Function 0x65
========================================================
*/
/**
 * @brief Отправка команды клавиши в терминал через функцию 0x65 (запись в файл 11)
 * @param slave_id  адрес ведомого
 * @param key_code  строка с escape-последовательностью (например, "\x1b[A")
 * @return true при успехе
 */
bool terminal_send_command(uint8_t slave_id,  const char *key_code) {
    uint16_t data_len = strlen(key_code);
    if (data_len == 0 || data_len > 252) {
        ESP_LOGE("TERMINAL", "Invalid data length: %d", data_len);
        return false;
    }

    uint8_t request[32 + data_len];
    int pos = 0;

    request[pos++] = slave_id;
    request[pos++] = 0x65;                           // функция записи в файл

    // FILE = 11 (терминал)
    request[pos++] = 0x00;
    request[pos++] = 0x0B;

    // OFFSET = 0
    request[pos++] = 0x00;
    request[pos++] = 0x00;
    request[pos++] = 0x00;
    request[pos++] = 0x00;

    // SIZE = data_len (4 байта, big-endian)
    request[pos++] = (data_len >> 24) & 0xFF;
    request[pos++] = (data_len >> 16) & 0xFF;
    request[pos++] = (data_len >> 8) & 0xFF;
    request[pos++] = data_len & 0xFF;

    // Копируем данные (без нулевого терминатора)
    memcpy(&request[pos], key_code, data_len);
    pos += data_len;

    // CRC
    mbcrc_insert_crc(request, pos);
    pos += 2;

    // Отправка
    int written = uart_bridge_send(request, pos);
    if (written != pos) {
        ESP_LOGE("TERMINAL", "Send failed for %d bytes", data_len);
        return false;
    }

    // Чтение ответа с таймаутом 300 мс
    uint8_t response[32];
    int len = uart_bridge_receive(response, sizeof(response), 200);
    if (len <= 0) {
        ESP_LOGE("TERMINAL", "No response to command 0x65");
        return false;
    }

    // Проверка CRC
    if (!mbcrc_is_valid(response, len)) {
        ESP_LOGE("TERMINAL", "CRC error in response");
        return false;
    }

    // Проверка кода функции
    if (response[1] != 0x65) {
        ESP_LOGE("TERMINAL", "Unexpected function code: 0x%02X", response[1]);
        return false;
    }

    // Проверка размера данных (должен быть 2)
    uint32_t resp_size = (response[8] << 24) | (response[9] << 16) | (response[10] << 8) | response[11];
    if (resp_size != 2) {
        ESP_LOGE("TERMINAL", "Wrong data size: %u", resp_size);
        return false;
    }

    //ESP_LOGI("TERMINAL", "Command 0x65 succeeded for key 0x%04X", key_code);
    return true;
}

/**
 * @brief Преобразует символьное имя клавиши (f1, up, enter) в escape-последовательность
 * @param cmd  строка-идентификатор (f1, up, enter и т.д.)
 * @return указатель на константную строку с escape-последовательностью или NULL
 */
const char*  get_key_code(const char *cmd)
{
    if (strcmp(cmd, "f1") == 0)      return "\x1b[11~";
    if (strcmp(cmd, "f2") == 0)      return "\x1b[12~";
    if (strcmp(cmd, "f3") == 0)      return "\x1b[13~";
    if (strcmp(cmd, "up") == 0)      return "\x1b[A";
    if (strcmp(cmd, "down") == 0)    return "\x1b[B";
    if (strcmp(cmd, "left") == 0)    return "\x1b[D";
    if (strcmp(cmd, "right") == 0)   return "\x1b[C";
    if (strcmp(cmd, "enter") == 0)   return "\r";
    if (strcmp(cmd, "esc") == 0)     return "q";  // два байта 0x12 0x34
    if (strcmp(cmd, "start") == 0)   return "5";
    if (strcmp(cmd, "stop") == 0)    return "6";
    return NULL;
}

/**
 * @brief Чтение файла через функцию 0x64 (произвольное чтение)
 * @param slave    адрес ведомого
 * @param offset   смещение в байтах
 * @param size     запрашиваемое количество байт (не более 4096)
 * @param timeout  таймаут в мс
 * @param out      буфер для данных
 * @param out_len  указатель для сохранения фактической длины
 * @return MODBUS_OK при успехе, при ошибке 0x03 (конец файла) возвращает MODBUS_OK с out_len=0
 */
modbus_status_t modbus_read_file_0x64(uint8_t slave, uint32_t offset, 
    uint32_t size,uint32_t timeout, uint8_t *out, uint16_t *out_len)
{
    uint16_t file_id = 1;
    if (size > 4096) {
        ESP_LOGE("MODBUS", "0x64: size too large: %u", size);
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
    request[pos++] = (crc >> 8) & 0xFF;

    ESP_LOGI("MODBUS", "0x64 request offset=%lu size=%lu", (unsigned long)offset, (unsigned long)size);
    // Убрано логирование всего буфера

    int written = uart_bridge_send(request, pos);
    if (written != pos) {
        ESP_LOGE("MODBUS", "0x64 send failed");
        return MODBUS_ERR_UART;
    }

    uint8_t response[4096];
    int len = uart_bridge_receive(response, sizeof(response), pdMS_TO_TICKS(timeout));
    if (len <= 0) {
        ESP_LOGE("MODBUS", "0x64 timeout");
        return MODBUS_ERR_TIMEOUT;
    }
    if (len < 12) {
        ESP_LOGE("MODBUS", "0x64 response too short");
        return MODBUS_ERR_UART;
    }
    if (!mbcrc_is_valid(response, len)) {
        ESP_LOGE("MODBUS", "0x64 CRC error");
        return MODBUS_ERR_CRC;
    }

    if (response[1] == 0xE4) {
        uint8_t err_code = response[2];
        ESP_LOGW("MODBUS", "0x64 error code: 0x%02X", err_code);
        if (err_code == 0x03) {
            *out_len = 0;
            return MODBUS_OK;
        }
        return MODBUS_ERR_UART;
    }
    if (response[1] != 0x64) {
        ESP_LOGE("MODBUS", "0x64 wrong function: 0x%02X", response[1]);
        return MODBUS_ERR_UART;
    }

    uint32_t resp_size = (response[8] << 24) | (response[9] << 16) | (response[10] << 8) | response[11];
    if (resp_size == 0) {
        *out_len = 0;
        return MODBUS_OK;
    }
    int data_offset = 12;
    int data_len = len - data_offset - 2;
    if (data_len <= 0) {
        ESP_LOGE("MODBUS", "0x64 no data");
        return MODBUS_ERR_UART;
    }
    if (data_len > resp_size) data_len = resp_size;
    memcpy(out, response + data_offset, data_len);
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

    int written = uart_bridge_send(request, pos);
    if (written != pos) {
        ESP_LOGE("MODBUS", "0x14 send failed");
        return MODBUS_ERR_UART;
    }

    uint8_t response[256];
    int len = uart_bridge_receive(response, sizeof(response), pdMS_TO_TICKS(3000));
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
        return MODBUS_OK;
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