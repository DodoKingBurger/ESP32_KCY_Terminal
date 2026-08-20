#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

void firmware_manager_init(void);

/* HTTP POST /firmware — тело файла прошивки */
esp_err_t firmware_manager_http_upload_handler(httpd_req_t *req);

/* WS JSON-команды: getFirmwareVersion, startReflash, … */
void firmware_manager_on_ws_command(const char *cmd);

/* Чтение версии по UART (заполняет out, true при успехе) */
bool firmware_manager_read_version(char *out, size_t out_len);

/* Команда перепрошивки по UART */
bool firmware_manager_send_reflash_command(void);

bool firmware_manager_is_busy(void);

#ifdef __cplusplus
}
#endif
