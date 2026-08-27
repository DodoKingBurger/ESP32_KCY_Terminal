#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Опрос телеметрии / задача терминала ---- */
void modbus_poll_task(void *arg);
void terminal_task_start(void);

/* ---- Терминал по UART (file 11, FC 0x64 / 0x65) ---- */
bool terminal_read_screen(
    uint8_t *screen,
    uint16_t *screen_len
);

bool terminal_send_command(uint8_t slave_id, const char *key_code);

const char *get_key_code(const char *cmd);

#ifdef __cplusplus
}
#endif
