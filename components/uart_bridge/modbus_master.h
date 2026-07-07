#pragma once

#include <stdint.h>

typedef enum
{
    MODBUS_OK = 0,

    MODBUS_ERR_TIMEOUT = -1,
    MODBUS_ERR_CRC = -2,
    MODBUS_ERR_UART = -3

} modbus_status_t;

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

uint16_t read_reg(uint16_t reg);

modbus_status_t modbus_write_single_register(
    uint8_t slave,
    uint16_t reg,
    uint16_t value
);

bool terminal_read_screen(
    uint8_t slave_id,
    uint8_t *screen,
    uint16_t *screen_len
);

bool terminal_send_command(uint8_t slave_id,  
    const char *key_code) ;

const char* get_key_code(const char *cmd);

// Добавить в конец файла
modbus_status_t modbus_read_file_0x14(uint8_t slave, uint16_t file_id, 
    uint16_t record_number, uint8_t *out, uint16_t *out_len);
bool modbus_read_archive_size(uint8_t slave, uint32_t *size);