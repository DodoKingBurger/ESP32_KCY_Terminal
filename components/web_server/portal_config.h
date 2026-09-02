#pragma once

/**
 * Captive portal (DNS-перехват всех имён + редиректы на портал).
 *
 * 0 — без captive portal:
 *     DHCP отдаёт DNS 192.168.4.1, быстрые ответы на /generate_204 и т.п.
 *     (ускоряет «Проверку сети» на телефоне, без перехвата всего трафика).
 *
 * 1 — полный captive portal:
 *     + dns_server (любое имя → 192.168.4.1)
 *     + доп. URI портала / 404 → корень.
 */
#ifndef CAPTIVE_PORTAL_ENABLE
#define CAPTIVE_PORTAL_ENABLE  0
#endif
