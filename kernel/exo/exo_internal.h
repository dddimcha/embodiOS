/* EMBODIOS exo-style distributed inference — внутренние интерфейсы модуля.
 *
 * Не является публичным API. Связывает файлы каталога kernel/exo между собой.
 */

#ifndef EMBODIOS_EXO_INTERNAL_H
#define EMBODIOS_EXO_INTERNAL_H

#include <embodios/exo.h>

/* --- Идентичность локальной ноды (exo_node.c) --- */

/* Локальная нода (node_id, ip, ctrl_port, ram, model_id). */
const exo_node_t* exo__self(void);

/* Текущий request_id счётчик (монотонный). */
uint64_t exo__next_request_id(void);

/* --- Оркестратор (exo_node.c), вызывается из exo_transport.c --- */

/* Входящее тензорное сообщение на открытом соединении fd.
 * payload может быть NULL, если hdr->payload_len == 0. */
void exo__handle_tensor_msg(int fd, const exo_tensor_msg_t *hdr,
                            const uint8_t *payload);

/* --- Генерация (exo_node.c), вызывается из exo_server.c --- */

/* Callback выдачи куска текста; done=true на последнем вызове. */
typedef void (*exo_emit_fn)(const char *text, bool done, void *ctx);

/* Сгенерировать ответ на prompt: локально, если нода одна/держит всю
 * модель, или запустив проход по кольцу. max_tokens <= 0 — дефолт
 * (EXO_DEFAULT_MAX_TOKENS). Возвращает 0 или код ошибки. */
int exo__chat(const char *prompt, exo_emit_fn emit, void *ctx,
              int max_tokens);

#endif /* EMBODIOS_EXO_INTERNAL_H */
