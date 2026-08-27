#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MODBUS_TIMEOUT_MS 200
#define SLAVE_ID            1
#define MAX_RETRIES      3
#define REFLASH_STATUS_REG 0x008A
/* 0x20 = 32 dec = КСУ Linux*/
#define FW_DEVICE_CODE_KSULINUX  0x20

typedef enum
{
    MODBUS_END_OF_FILE = 1,
    MODBUS_OK = 0,
    MODBUS_ERR_TIMEOUT = -1,
    MODBUS_ERR_CRC = -2,
    MODBUS_ERR_UART = -3,
    MODBUS_ERR = -4
} modbus_status_t;

/* ---- Общие Modbus-функции (holding / input / file / archive) ---- */

modbus_status_t modbus_read_holding(
    uint16_t reg,
    uint16_t count,
    uint8_t *response,
    uint16_t *response_len
);

modbus_status_t modbus_read_input(
    uint16_t reg,
    uint16_t count,
    uint8_t *response,
    uint16_t *response_len
);

modbus_status_t modbus_read_registers(
    uint16_t start_reg,
    uint16_t count,
    uint16_t *values
);

modbus_status_t modbus_read_file_0x64(uint32_t offset,
    uint32_t size, uint8_t *out, uint16_t *out_len);

/** use_classic_uart: true = путь как у архива; false = long_tx (сброс эха) */
modbus_status_t modbus_write_file_0x65(uint16_t file_id,
    uint32_t offset, const uint8_t *data, uint32_t data_len,
    bool use_classic_uart);

/** Деталь последней ошибки 0x65 (до следующего вызова) */
const char *modbus_write_file_0x65_last_error(void);

modbus_status_t modbus_read_file_0x14(uint16_t file_id,
    uint16_t record_number, uint8_t *out, uint16_t *out_len);

bool modbus_read_archive_size(uint32_t *size);
