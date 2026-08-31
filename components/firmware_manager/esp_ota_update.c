/**
 * OTA обновление прошивки ESP32 через SoftAP HTTP.
 * Не трогает UART / КСУ / архив — только flash OTA-слот.
 */

#include "esp_ota_update.h"
#include "firmware_manager.h"
#include "web_server.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_system.h"
#include "esp_http_server.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>

#define TAG "ESP_OTA"
#define OTA_CHUNK 4096

static bool ota_busy = false;

void esp_ota_update_init(void)
{
    /* резерв под будущую инициализацию */
}

bool esp_ota_update_is_busy(void)
{
    return ota_busy;
}

static void send_log(const char *msg)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"type\":\"log\",\"msg\":\"%s\"}", msg);
    web_server_send(buf);
}

static void send_progress(int received, int total)
{
    char prog[160];
    float pct = total > 0 ? (100.0f * received / total) : 0;
    snprintf(prog, sizeof(prog),
             "{\"type\":\"firmwareProgress\",\"received\":%d,\"total\":%d,\"percent\":%.1f}",
             received, total, pct);
    web_server_send(prog);
}

esp_err_t esp_ota_update_http_handler(httpd_req_t *req)
{
    if (ota_busy || firmware_manager_is_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_sendstr(req, "Firmware/OTA busy");
        return ESP_OK;
    }

    int total = req->content_len;
    if (total <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_sendstr(req, "Empty body");
        return ESP_OK;
    }

    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (update_part == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_sendstr(req, "No OTA partition (check partitions.csv)");
        send_log("OTA: no next update partition");
        return ESP_OK;
    }

    if ((size_t)total > update_part->size) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_sendstr(req, "Image larger than OTA slot");
        send_log("OTA: image too large for slot");
        return ESP_OK;
    }

    ota_busy = true;
    send_log("ESP OTA started");
    {
        char msg[120];
        snprintf(msg, sizeof(msg),
                 "{\"type\":\"firmwareUploadStart\",\"size\":%d,\"target\":\"esp\"}", total);
        web_server_send(msg);
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ota_busy = false;
        char em[96];
        snprintf(em, sizeof(em), "esp_ota_begin failed: %s", esp_err_to_name(err));
        send_log(em);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_sendstr(req, em);
        return ESP_OK;
    }

    static uint8_t buf[OTA_CHUNK];
    int received = 0;
    bool ok = true;
    const char *fail_msg = "write failed";

    while (received < total) {
        int to_read = total - received;
        if (to_read > (int)sizeof(buf)) to_read = (int)sizeof(buf);

        int r = httpd_req_recv(req, (char *)buf, to_read);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            fail_msg = "HTTP aborted";
            ok = false;
            break;
        }

        err = esp_ota_write(ota_handle, buf, (size_t)r);
        if (err != ESP_OK) {
            fail_msg = esp_err_to_name(err);
            ok = false;
            break;
        }

        received += r;
        /* не чаще чем раз на ~32 КБ, чтобы не забить SoftAP */
        if ((received & 0x7FFF) < r || received >= total) {
            send_progress(received, total);
        }
    }

    httpd_resp_set_hdr(req, "Connection", "close");

    if (!ok || received < total) {
        esp_ota_abort(ota_handle);
        ota_busy = false;
        char body[160];
        snprintf(body, sizeof(body),
                 "{\"ok\":false,\"msg\":\"OTA %s\",\"received\":%d}",
                 fail_msg ? fail_msg : "fail", received);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, body);
        web_server_send("{\"type\":\"firmwareUploadError\",\"msg\":\"ESP OTA failed\"}");
        send_log("ESP OTA failed");
        return ESP_OK;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ota_busy = false;
        char em[96];
        snprintf(em, sizeof(em), "esp_ota_end: %s", esp_err_to_name(err));
        send_log(em);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"ota_end failed\"}");
        return ESP_OK;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ota_busy = false;
        send_log("esp_ota_set_boot_partition failed");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"set_boot failed\"}");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    web_server_send("{\"type\":\"firmwareUploadComplete\",\"size\":0,\"target\":\"esp\",\"reboot\":true}");
    send_log("ESP OTA OK — reboot in 1s");

    ota_busy = false;
    /* дать HTTP-ответ уйти */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}
