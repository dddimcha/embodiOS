/* EMBODIOS exo-style distributed inference — OpenAI-совместимый HTTP API.
 *
 * Минимальный аналог exo/api/chatgpt_api.py (классическая exo, порт 52415):
 *
 *   POST /v1/chat/completions   {"model":..., "messages":[{"role","content"}],
 *                                "stream":bool}
 *                               → application/json или SSE (text/event-stream,
 *                                 чанки chat.completion.chunk, финал [DONE])
 *   GET  /v1/models             {"data":[{"id":...}]}
 *   GET  /healthcheck           {"status":"ok"}
 *
 * HTTP-парсер — намеренно минимальный (request line, Content-Length,
 * тело как JSON «плоским» поиском полей). Этого достаточно для curl и
 * OpenAI SDK в простых сценариях; полноценный JSON-парсер — TODO.
 *
 * Ограничения tcpip.c (как в exo_transport.c): socket_accept() отдаёт тот же
 * fd, поэтому после каждого соединения listener пересоздаётся; отправка
 * чанками <= EXO_HTTP_CHUNK (MTU стека — один кадр на socket_send).
 */

#include <embodios/exo.h>
#include <embodios/tcpip.h>
#include <embodios/console.h>
#include <embodios/hal_timer.h>
#include <string.h>
#include <stdio.h>

#include "exo_internal.h"

#define EXO_HTTP_CHUNK      1400
#define EXO_HTTP_MAX_REQ    (16 * 1024)
#define EXO_HTTP_MAX_BODY   (8 * 1024)
#define EXO_HTTP_TIMEOUT_MS 5000

static int g_server_fd = -1;
static int g_server_port = 0;
static uint64_t g_req_counter = 0;

/* ============================================================================
 * HTTP helpers
 * ============================================================================ */

static int http_write_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > EXO_HTTP_CHUNK) chunk = EXO_HTTP_CHUNK;
        int ret = socket_send(fd, buf + sent, chunk);
        if (ret < 0) {
            if (ret == NET_ERR_UNREACHABLE) { tcpip_poll(); continue; }
            return EXO_ERR_NET;
        }
        sent += chunk;
        tcpip_poll();
    }
    return EXO_OK;
}

static int http_write_str(int fd, const char *s)
{
    return http_write_all(fd, s, strlen(s));
}

/* Прочитать HTTP-запрос целиком: заголовки до \r\n\r\n + тело по
 * Content-Length. Возвращает длину или <0. */
static int http_read_request(int fd, char *buf, size_t cap)
{
    size_t len = 0;
    size_t body_want = 0;   /* 0 = заголовки ещё не закончились */
    size_t hdr_len = 0;
    uint64_t start = hal_timer_get_milliseconds();

    for (;;) {
        int n = socket_recv(fd, buf + len, cap - 1 - len);
        if (n > 0) {
            len += (size_t)n;
            buf[len] = '\0';

            if (!hdr_len) {
                char *end = strstr(buf, "\r\n\r\n");
                if (end) {
                    hdr_len = (size_t)(end - buf) + 4;
                    /* Content-Length (регистронезависимо не ищем —
                     * OpenAI SDK и curl шлют каноничный вид) */
                    char *cl = strstr(buf, "Content-Length:");
                    if (cl) {
                        body_want = 0;
                        const char *p = cl + 15;
                        while (*p >= '0' && *p <= '9') {
                            body_want = body_want * 10 + (size_t)(*p - '0');
                            p++;
                        }
                        if (body_want > EXO_HTTP_MAX_BODY)
                            body_want = EXO_HTTP_MAX_BODY;
                    }
                }
            }
            if (hdr_len && len >= hdr_len + body_want)
                return (int)len;
        } else {
            if (hal_timer_get_milliseconds() - start > EXO_HTTP_TIMEOUT_MS)
                return EXO_ERR_TIMEOUT;
            tcpip_poll();
        }
        if (len >= cap - 1)
            return (int)len;
    }
}

/* ============================================================================
 * Наивный JSON: извлечь последнее "content":"...", "model":"...", "stream"
 * ============================================================================ */

static int json_last_str(const char *json, const char *key,
                         char *out, size_t cap)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *last = NULL;
    const char *p = json;
    while ((p = strstr(p, pat)) != NULL) {
        last = p + strlen(pat);
        p = last;
    }
    if (!last) return -1;
    size_t n = 0;
    while (last[n] && last[n] != '"' && n < cap - 1) {
        /* минимальная обработка escapes */
        if (last[n] == '\\' && last[n + 1] == 'n') { out[n++] = '\n'; last++; }
        else if (last[n] == '\\' && last[n + 1])     { out[n] = last[n + 1]; last++; }
        else                                           out[n] = last[n];
        n++;
    }
    out[n] = '\0';
    return 0;
}

/* Экранирование строки для JSON-ответа */
static size_t json_escape(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    for (const char *p = in; *p && n < cap - 2; p++) {
        if (*p == '"' || *p == '\\') {
            out[n++] = '\\';
            out[n++] = *p;
        } else if (*p == '\n') {
            out[n++] = '\\';
            out[n++] = 'n';
        } else {
            out[n++] = *p;
        }
    }
    out[n] = '\0';
    return n;
}

/* ============================================================================
 * SSE-стрим ответа
 * ============================================================================ */

typedef struct {
    int      fd;
    char     model[EXO_MODEL_ID_LEN];
    uint64_t created;
    int      index;
} sse_ctx_t;

static void sse_emit(const char *text, bool done, void *opaque)
{
    sse_ctx_t *ctx = (sse_ctx_t *)opaque;
    char esc[512];
    char chunk[768];

    if (!done) {
        json_escape(text, esc, sizeof(esc));
        int len = snprintf(chunk, sizeof(chunk),
            "data: {\"id\":\"chatcmpl-exo-%u\",\"object\":\"chat.completion.chunk\","
            "\"created\":%u,\"model\":\"%s\",\"system_fingerprint\":\"exo_embodios\","
            "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"%s\"},"
            "\"finish_reason\":null}]}\n\n",
            (unsigned)g_req_counter, (unsigned)ctx->created,
            ctx->model, esc);
        if (len > 0) http_write_all(ctx->fd, chunk, (size_t)len);
    } else {
        int len = snprintf(chunk, sizeof(chunk),
            "data: {\"id\":\"chatcmpl-exo-%u\",\"object\":\"chat.completion.chunk\","
            "\"created\":%u,\"model\":\"%s\",\"choices\":[{\"index\":0,"
            "\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
            "data: [DONE]\n\n",
            (unsigned)g_req_counter, (unsigned)ctx->created, ctx->model);
        if (len > 0) http_write_all(ctx->fd, chunk, (size_t)len);
    }
}

/* Накапливающий emit для непотокового ответа.
 * ctx — массив указателей: [0]=buf, [1]=текущая позиция, [2]=конец буфера. */
static void accum_emit(const char *text, bool done, void *opaque)
{
    (void)done;
    char **p = (char **)opaque;
    char *cur = p[1];
    while (*text && cur < p[2] - 1)
        *cur++ = *text++;
    *cur = '\0';
    p[1] = cur;
}

/* Извлечь целочисленное поле ("key":N); 0 если не найдено */
static int json_get_int(const char *json, const char *key)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    int v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
    }
    return v;
}

static size_t chat_accumulate(const char *prompt, int max_tokens,
                              char *buf, size_t cap)
{
    char *ctx[3] = { buf, buf, buf + cap };
    exo__chat(prompt, accum_emit, ctx, max_tokens);
    return (size_t)(ctx[1] - buf);
}

/* ============================================================================
 * Обработчики эндпоинтов
 * ============================================================================ */

static void respond_json(int fd, int code, const char *status,
                         const char *body)
{
    char hdr[256];
    int len = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %u\r\nConnection: close\r\n\r\n",
        code, status, (unsigned)strlen(body));
    if (len > 0) http_write_all(fd, hdr, (size_t)len);
    http_write_str(fd, body);
}

/* Real model id: prefer the loaded GGUF's general.name (the model may be
 * loaded after exo_init), then the node record, then "unknown". */
static const char *current_model_id(void)
{
    extern const char *gguf_get_model_name(void);
    const char *name = gguf_get_model_name();
    if (name && name[0]) return name;
    const exo_node_t *self = exo__self();
    if (self && self->model_id[0]) return self->model_id;
    return "unknown";
}

static void handle_models(int fd)
{
    char body[256];
    snprintf(body, sizeof(body),
             "{\"object\":\"list\",\"data\":[{\"id\":\"%s\","
             "\"object\":\"model\",\"owned_by\":\"embodios-exo\"}]}",
             current_model_id());
    respond_json(fd, 200, "OK", body);
}

static void handle_chat_completions(int fd, const char *body, bool stream)
{
    char prompt[EXO_HTTP_MAX_BODY / 2];
    char model[EXO_MODEL_ID_LEN];

    if (json_last_str(body, "content", prompt, sizeof(prompt)) != 0) {
        respond_json(fd, 400, "Bad Request",
            "{\"error\":{\"message\":\"messages[].content not found\"}}");
        return;
    }
    if (json_last_str(body, "model", model, sizeof(model)) != 0) {
        strncpy(model, current_model_id(), sizeof(model) - 1);
        model[sizeof(model) - 1] = '\0';
    }

    int max_tokens = json_get_int(body, "max_tokens");  /* 0 = дефолт */

    g_req_counter++;
    console_printf("[EXO] chat/completions model='%s' stream=%d max_tokens=%d\n",
                   model, stream ? 1 : 0, max_tokens);

    if (stream) {
        /* SSE: заголовок + поток чанков */
        http_write_str(fd,
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\nConnection: close\r\n\r\n");
        sse_ctx_t ctx;
        ctx.fd = fd;
        strncpy(ctx.model, model, sizeof(ctx.model) - 1);
        ctx.model[sizeof(ctx.model) - 1] = '\0';
        ctx.created = hal_timer_get_milliseconds() / 1000;
        ctx.index = 0;
        exo__chat(prompt, sse_emit, &ctx, max_tokens);
    } else {
        /* Непотоковый ответ: собрать весь текст в буфер */
        static char acc[4096];
        chat_accumulate(prompt, max_tokens, acc, sizeof(acc));

        char esc[4096];
        json_escape(acc, esc, sizeof(esc));
        char resp[4608];
        int len = snprintf(resp, sizeof(resp),
            "{\"id\":\"chatcmpl-exo-%u\",\"object\":\"chat.completion\","
            "\"created\":%u,\"model\":\"%s\",\"system_fingerprint\":\"exo_embodios\","
            "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
            "\"content\":\"%s\"},\"finish_reason\":\"stop\"}]}",
            (unsigned)g_req_counter,
            (unsigned)(hal_timer_get_milliseconds() / 1000), model, esc);
        if (len > 0) respond_json(fd, 200, "OK", resp);
    }
}

/* ============================================================================
 * Публичный API
 * ============================================================================ */

int exo_serve_openai(uint16_t port)
{
    if (g_server_fd >= 0) return EXO_OK;

    if (port == 0) port = EXO_DEFAULT_API_PORT;

    int fd = socket_create(SOCK_STREAM, IP_PROTO_TCP);
    if (fd < 0) return EXO_ERR_NET;
    if (socket_bind(fd, 0, port) != NET_OK ||
        socket_listen(fd, 1) != NET_OK) {
        socket_close(fd);
        return EXO_ERR_NET;
    }

    g_server_fd = fd;
    g_server_port = (int)port;
    console_printf("[EXO] OpenAI API on http://0.0.0.0:%u/v1\n",
                   (unsigned)port);
    return EXO_OK;
}

void exo_serve_stop(void)
{
    if (g_server_fd < 0) return;
    socket_close(g_server_fd);
    console_printf("[EXO] OpenAI API on :%d stopped\n", g_server_port);
    g_server_fd = -1;
    g_server_port = 0;
}

bool exo_server_running(void)
{
    return g_server_fd >= 0;
}

uint16_t exo_server_port(void)
{
    return (uint16_t)g_server_port;
}

void exo_server_poll(void)
{
    if (g_server_fd < 0) return;

    /* TODO(tcpip.c): публичный геттер состояния сокета */
    socket_t *s = tcpip_get_socket_for_testing(g_server_fd);
    if (!s || !s->active)
        return;
    /* SYN получен (SYN_RECEIVED) или handshake завершён (ESTABLISHED) */
    if (s->state != TCP_SYN_RECEIVED && s->state != TCP_ESTABLISHED)
        return;

    uint32_t remote_ip = 0;
    uint16_t remote_port = 0;
    int conn = socket_accept(g_server_fd, &remote_ip, &remote_port);
    if (conn < 0) return;

    static char req[EXO_HTTP_MAX_REQ];
    int len = http_read_request(conn, req, sizeof(req));
    if (len > 0) {
        if (strncmp(req, "POST /v1/chat/completions", 25) == 0) {
            char *body = strstr(req, "\r\n\r\n");
            bool stream = body && strstr(body, "\"stream\":true");
            handle_chat_completions(conn, body ? body + 4 : "", stream);
        } else if (strncmp(req, "GET /v1/models", 14) == 0) {
            handle_models(conn);
        } else if (strncmp(req, "GET /healthcheck", 16) == 0) {
            respond_json(conn, 200, "OK", "{\"status\":\"ok\"}");
        } else {
            respond_json(conn, 404, "Not Found",
                "{\"error\":{\"message\":\"unknown endpoint\"}}");
        }
    }

    socket_close(conn);

    /* listener пересоздаём (accept переиспользовал fd) */
    g_server_fd = -1;
    exo_serve_openai((uint16_t)g_server_port);
}
