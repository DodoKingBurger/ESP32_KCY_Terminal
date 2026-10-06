#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Порт raw-TCP терминала для IRZ-Terminal (ANSI экран + native key bytes). */
#define TERMINAL_TCP_PORT 8888

/**
 * @brief Запускает TCP-сервер терминала (порт TERMINAL_TCP_PORT).
 *        Принимает одного клиента: отдаёт ANSI-экран, принимает команды клавиш.
 *        Не влияет на WebSocket /ws и captive portal.
 */
void terminal_tcp_start(void);

/**
 * @brief true, если к TCP-мосту подключён клиент (IRZ-Terminal).
 */
bool terminal_tcp_client_connected(void);

/**
 * @brief Отправляет кадр экрана (ANSI) активному TCP-клиенту, если он есть.
 */
void terminal_tcp_send(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
