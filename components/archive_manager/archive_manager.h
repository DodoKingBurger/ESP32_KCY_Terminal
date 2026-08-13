#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QUEUE_SIZE 32
#define QUEUE_CHUNK_SIZE 2000

typedef struct {
    uint8_t data[QUEUE_CHUNK_SIZE];
    uint16_t len;
    uint32_t offset;
} chunk_queue_item_t;

/* Глобальные флаги состояния загрузки (доступны и из других модулей) */
extern bool download_in_progress;
extern volatile bool download_stop_requested;

/* Инициализация модуля */
void archive_manager_init(void);

/* Обработчик HTTP GET /download, отдающий собранный архив чанками */
esp_err_t archive_manager_http_download_handler(httpd_req_t *req);

/* Обработка входящих JSON-команд от ws_handler */
void archive_manager_on_ws_command(const char *cmd);

/* Потокобезопасный snapshot текущего имени/размера архива */
size_t archive_manager_get_size(void);
void   archive_manager_get_name(char *out, size_t out_size);

/* Проверка, выполняется ли загрузка */
bool archive_manager_is_downloading(void);

/* Остановка загрузки */
void archive_manager_stop_download(void);

#ifdef __cplusplus
}
#endif