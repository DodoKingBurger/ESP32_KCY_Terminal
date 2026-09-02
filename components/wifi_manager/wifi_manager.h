#pragma once

#include <stdbool.h>

void wifi_manager_start(void);

/** Текущий SSID SoftAP (после wifi_manager_start) */
const char *wifi_manager_get_ap_ssid(void);

/**
 * Перечитать 0x040D/0x040E/0x040F и при изменении обновить SSID SoftAP.
 * @return true если SSID реально сменили
 */
bool wifi_manager_refresh_ssid(void);
