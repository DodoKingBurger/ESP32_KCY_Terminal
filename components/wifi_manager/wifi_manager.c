#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"
#include "lwip/inet.h"
#include "web_server.h"
#include "modbus_master.h"
#include "firmware_manager.h"
#include "esp_ota_update.h"
#include "archive_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

#define TAG "WIFI"

#define AP_SSID_FALLBACK "ESP32-Portal"
#define AP_PASS          "12345678"

#define REG_MESTOR  0x040C
#define REG_KUST    0x040D
#define REG_SKV     0x040E

/* Как часто перечитывать площадку (мс) — только опрос, без пересоздания AP */
#define SSID_REFRESH_MS  1000

static char s_ap_ssid[33];           /* текущий SSID SoftAP */
static char s_pending_ssid[33];      /* кандидат с КСУ; применить после отключения клиентов */
static bool s_wifi_started;
static int  s_sta_count;             /* число подключённых станций */
static SemaphoreHandle_t s_ssid_mu;


static bool read_holding_u16_n(uint16_t start, uint16_t count, uint16_t *out)
{
    if (count == 0 || count > 16 || out == NULL) {
        return false;
    }
    uint8_t resp[64];
    uint16_t resp_len = 0;
    modbus_status_t st = modbus_read_holding(start, count, resp, &resp_len);
    if (st != MODBUS_OK) {
        ESP_LOGW(TAG, "FC03 read 0x%04X count=%u st=%d", (unsigned)start, (unsigned)count, (int)st);
        return false;
    }
    if (resp_len < (uint16_t)(3 + count * 2) || resp[1] != 0x03 || resp[2] < count * 2) {
        ESP_LOGW(TAG, "FC03 bad frame len=%u", (unsigned)resp_len);
        return false;
    }
    for (uint16_t i = 0; i < count; i++) {
        out[i] = (uint16_t)((resp[3 + i * 2] << 8) | resp[4 + i * 2]);
    }
    return true;
}

static void build_ap_ssid(char *dst, size_t dst_sz, uint16_t mestor, uint16_t kust, uint16_t skv)
{
    if (dst_sz == 0) {
        return;
    }
    int n = snprintf(dst, dst_sz, "IRZ500 Field%u Cluster%u well%u",
                     (unsigned)mestor, (unsigned)kust, (unsigned)skv);
    if (n < 0 || (size_t)n >= dst_sz || strlen(dst) > 32) {
        snprintf(dst, dst_sz, "ESP_%u_%u_%u",
                 (unsigned)mestor, (unsigned)kust, (unsigned)skv);
        if (strlen(dst) > 32) {
            dst[32] = '\0';
        }
    }
}

/** Собрать кандидат SSID из КСУ; false — опрос не удался */
static bool fetch_ssid_from_ksu(char *out, size_t out_sz)
{
    uint16_t regs[3] = {0, 0, 0};
    if (!read_holding_u16_n(REG_MESTOR, 3, regs)) {
        return false;
    }
    build_ap_ssid(out, out_sz, regs[0], regs[1], regs[2]);
    ESP_LOGI(TAG, "Location FC03: mestor=%u kust=%u skv=%u → \"%s\"",
             (unsigned)regs[0], (unsigned)regs[1], (unsigned)regs[2], out);
    return true;
}

static void resolve_ap_ssid(void)
{
    char tmp[33];
    bool ok = false;
    for (int attempt = 0; attempt < 5; attempt++) {
        if (fetch_ssid_from_ksu(tmp, sizeof(tmp))) {
            ok = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (ok) {
        strncpy(s_ap_ssid, tmp, sizeof(s_ap_ssid) - 1);
        s_ap_ssid[sizeof(s_ap_ssid) - 1] = '\0';
    } else {
        strncpy(s_ap_ssid, AP_SSID_FALLBACK, sizeof(s_ap_ssid) - 1);
        s_ap_ssid[sizeof(s_ap_ssid) - 1] = '\0';
        ESP_LOGW(TAG, "Location read failed → SSID fallback \"%s\"", s_ap_ssid);
    }
}

/** Применить SSID к уже запущенному SoftAP */
static bool apply_ap_ssid(const char *ssid)
{
    if (!s_wifi_started || ssid == NULL || ssid[0] == '\0') {
        return false;
    }
    wifi_config_t cfg = {0};
    if (esp_wifi_get_config(WIFI_IF_AP, &cfg) != ESP_OK) {
        return false;
    }
    memset(cfg.ap.ssid, 0, sizeof(cfg.ap.ssid));
    strncpy((char *)cfg.ap.ssid, ssid, sizeof(cfg.ap.ssid) - 1);
    cfg.ap.ssid_len = (uint8_t)strlen((char *)cfg.ap.ssid);
    strncpy((char *)cfg.ap.password, AP_PASS, sizeof(cfg.ap.password) - 1);
    cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    cfg.ap.channel = 1;
    cfg.ap.max_connection = 4;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "SoftAP SSID updated → \"%s\"", ssid);
    return true;
}

const char *wifi_manager_get_ap_ssid(void)
{
    return s_ap_ssid[0] ? s_ap_ssid : AP_SSID_FALLBACK;
}

/**
 * Опрос площадки с КСУ.
 * Если SSID изменился, а клиенты ещё подключены — только запоминаем pending.
 * Применяем к SoftAP, когда станций 0 (пользователь отключился).
 */
bool wifi_manager_refresh_ssid(void)
{
    if (!s_wifi_started) {
        return false;
    }
    /* Не мешаем длинным UART-операциям */
    if (firmware_manager_is_busy() || esp_ota_update_is_busy()) {
        return false;
    }
    if (download_in_progress) {
        return false;
    }

    char neu[33];
    if (!fetch_ssid_from_ksu(neu, sizeof(neu))) {
        return false;
    }

    if (s_ssid_mu) {
        xSemaphoreTake(s_ssid_mu, portMAX_DELAY);
    }

    bool applied = false;
    bool same_as_current = (strncmp(s_ap_ssid, neu, sizeof(s_ap_ssid)) == 0);

    if (same_as_current) {
        /* Актуальный SSID совпадает — pending не нужен */
        s_pending_ssid[0] = '\0';
    } else if (s_sta_count > 0) {
        /* Клиенты онлайн: не трогаем AP, копим новое имя */
        if (strncmp(s_pending_ssid, neu, sizeof(s_pending_ssid)) != 0) {
            strncpy(s_pending_ssid, neu, sizeof(s_pending_ssid) - 1);
            s_pending_ssid[sizeof(s_pending_ssid) - 1] = '\0';
            ESP_LOGI(TAG,
                     "SSID pending \"%s\" (clients=%d) — apply after disconnect",
                     s_pending_ssid, s_sta_count);
        }
    } else {
        /* Никого нет — можно сразу обновить SoftAP */
        strncpy(s_ap_ssid, neu, sizeof(s_ap_ssid) - 1);
        s_ap_ssid[sizeof(s_ap_ssid) - 1] = '\0';
        s_pending_ssid[0] = '\0';
        applied = apply_ap_ssid(s_ap_ssid);
    }

    if (s_ssid_mu) {
        xSemaphoreGive(s_ssid_mu);
    }
    return applied;
}

/** Применить отложенный SSID, если клиенты ушли */
static void try_apply_pending_ssid(void)
{
    if (!s_wifi_started || s_sta_count > 0) {
        return;
    }
    if (s_ssid_mu) {
        xSemaphoreTake(s_ssid_mu, portMAX_DELAY);
    }
    if (s_pending_ssid[0] != '\0' &&
        strncmp(s_ap_ssid, s_pending_ssid, sizeof(s_ap_ssid)) != 0) {
        strncpy(s_ap_ssid, s_pending_ssid, sizeof(s_ap_ssid) - 1);
        s_ap_ssid[sizeof(s_ap_ssid) - 1] = '\0';
        s_pending_ssid[0] = '\0';
        ESP_LOGI(TAG, "Applying pending SSID after last client left → \"%s\"",
                 s_ap_ssid);
        apply_ap_ssid(s_ap_ssid);
    } else {
        s_pending_ssid[0] = '\0';
    }
    if (s_ssid_mu) {
        xSemaphoreGive(s_ssid_mu);
    }
}

static void ssid_refresh_task(void *arg)
{
    (void)arg;
    /* первая проверка не сразу после старта */
    vTaskDelay(pdMS_TO_TICKS(SSID_REFRESH_MS));
    for (;;) {
        wifi_manager_refresh_ssid();
        vTaskDelay(pdMS_TO_TICKS(SSID_REFRESH_MS));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    switch (event_id) {
        case WIFI_EVENT_AP_STACONNECTED:
            s_sta_count++;
            ESP_LOGI(TAG, "Client connected (sta=%d)", s_sta_count);
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            if (s_sta_count > 0) {
                s_sta_count--;
            }
            ESP_LOGI(TAG, "Client disconnected (sta=%d)", s_sta_count);
            web_server_client_disconnected();
            /* Точка пересоздаётся / SSID меняется только когда никто не подключён */
            try_apply_pending_ssid();
            break;
        default:
            break;
    }
}

void wifi_manager_start(void)
{
    if (s_ssid_mu == NULL) {
        s_ssid_mu = xSemaphoreCreateMutex();
    }
    s_sta_count = 0;
    s_pending_ssid[0] = '\0';

    resolve_ap_ssid();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    assert(ap_netif);

    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));

    esp_netif_dns_info_t dns;
    IP4_ADDR(&dns.ip.u_addr.ip4, 192, 168, 4, 1);
    dns.ip.type = IPADDR_TYPE_V4;
    ESP_ERROR_CHECK(esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns));

    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_restore());

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.ap.ssid, s_ap_ssid, sizeof(wifi_config.ap.ssid) - 1);
    strncpy((char *)wifi_config.ap.password, AP_PASS, sizeof(wifi_config.ap.password) - 1);
    wifi_config.ap.ssid_len = (uint8_t)strlen((char *)wifi_config.ap.ssid);
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_wifi_started = true;

    ESP_LOGI(TAG, "Access point started SSID=\"%s\" pass=\"%s\"",
             wifi_manager_get_ap_ssid(), AP_PASS);

    xTaskCreate(ssid_refresh_task, "ssid_refresh", 3072, NULL, 5, NULL);
}
