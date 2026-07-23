#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

extern bool load_page_active;
extern bool download_in_progress;
extern volatile bool download_stop_requested;

void web_server_start(void);
void web_server_send(const char *data);
void web_server_client_disconnected(void);

void web_server_send_binary(const uint8_t *data, size_t len);

size_t web_server_get_archive_size(void);

typedef struct {
    char type[8];
    uint32_t value;
} ws_command_t;
