#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

void firmware_manager_init(void);

/* HTTP POST /firmware — тело файла прошивки (уже с header + FFFFFFFF) */
esp_err_t firmware_manager_http_upload_handler(httpd_req_t *req);

/* WS: getFirmwareVersion, setDeviceCode, startReflash, getReflashStatus */
void firmware_manager_on_ws_command(const char *cmd);

bool firmware_manager_read_version(char *out, size_t out_len);

/* FC 0x06 reg 0x0000, value = device code */
bool firmware_manager_send_reflash_command(void);

bool firmware_manager_is_busy(void);

#ifdef __cplusplus
}
#endif
