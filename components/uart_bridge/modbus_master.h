#pragma once

#include <stdint.h>
#include <stdbool.h>

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
    uint8_t slave,
    uint16_t reg,
    uint16_t count,
    uint8_t *response,
    uint16_t *response_len
);

modbus_status_t modbus_read_input(
    uint8_t slave,
    uint16_t reg,
    uint16_t count,
    uint8_t *response,
    uint16_t *response_len
);

modbus_status_t modbus_read_registers(
    uint8_t slave,
    uint16_t start_reg,
    uint16_t count,
    uint16_t *values
);

/** Holding registers (FC 0x03), распаковка в uint16_t[] */
modbus_status_t modbus_read_holding_registers(
    uint8_t slave,
    uint16_t start_reg,
    uint16_t count,
    uint16_t *values
);

modbus_status_t modbus_write_single_register(
    uint8_t slave,
    uint16_t reg,
    uint16_t value
);

modbus_status_t modbus_read_file_0x64(uint8_t slave, uint32_t offset,
    uint32_t size, uint8_t *out, uint16_t *out_len);

modbus_status_t modbus_read_file_0x14(uint8_t slave, uint16_t file_id,
    uint16_t record_number, uint8_t *out, uint16_t *out_len);

bool modbus_read_archive_size(uint8_t slave, uint32_t *size);
