#pragma once

/**
 * OTA-перепрошивка самой ESP32 по Wi‑Fi (HTTP POST /ota).
 * Логика КСУ (0x65/0x06) — в firmware_manager.c; новые цели — отдельные модули.
 */

#include <stdbool.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

void esp_ota_update_init(void);

/** true пока идёт запись образа в flash */
bool esp_ota_update_is_busy(void);

/**
 * HTTP POST /ota — тело = app-образ (build/<project>.bin).
 * Пишет во второй OTA-слот, по успеху отвечает JSON и перезагружает ESP.
 */
esp_err_t esp_ota_update_http_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
