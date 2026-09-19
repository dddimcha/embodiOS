/* EMBODIOS exo-style distributed inference — тензорный транспорт (TCP).
 *
 * Замена gRPC NodeService из классической exo: вместо protobuf — бинарный
 * length-prefixed протокол поверх сокетов net/tcpip.c:
 *
 *   [48 байт exo_tensor_msg_t, network byte order][payload_len байт payload]
 *
 * Сообщения: PROMPT (токены int32), TENSOR (активации f32/f16),
 * RESULT (токен + флаг is_finished), HEALTH, TOPOLOGY.
 *
 * Ограничения текущего tcpip.c, учтённые/задокументированные здесь:
 *  - tx_buffer в tcpip.c — один кадр ETH_FRAME_MAX (net/tcpip.c:35), поэтому
 *    socket_send() с len > ~1440 байт переполнит буфер. Мы режем поток на
 *    чанки EXO_TCP_CHUNK (TODO(tcpip.c): реальная сегментация/MSS).
 *  - rx_buffer сокета — 4096 байт (SOCKET_BUFFER_SIZE, tcpip.h:161),
 *    окно 8192, нет переупорядочивания сегментов: отправитель обязан слать
 *    маленькими чанками, получатель — дренировать вовремя. Для 32B hidden
 *    (12 КБ f16) это ~9 чанков на токен на hop — приемлемо.
 *  - socket_connect() только шлёт SYN; handshake завершается асинхронно в
 *    tcpip_poll() — ждём ESTABLISHED через tcpip_get_socket_for_testing().
 *    TODO(tcpip.c): нужен публичный геттер состояния сокета.
 *  - socket_accept() возвращает тот же fd (tcpip.c:964): один listen-сокет
 *    обслуживает одно соединение, после accept пересоздаём listener.
 *  - ARP-промах на первом пакете → NET_ERR_UNREACHABLE: повторяем после
 *    tcpip_poll() (ARP reply придёт асинхронно).
 */

#include <embodios/exo.h>
#include <embodios/tcpip.h>
#include <embodios/console.h>
#include <embodios/hal_timer.h>
#include <string.h>

#include "exo_internal.h"

/* Макс. чанк TCP-потока: MTU 1500 - eth(14) - ip(20) - tcp(20) = 1446,
 * с запасом 1400. */
#define EXO_TCP_CHUNK       1400
#define EXO_CONNECT_TIMEOUT_MS  3000
#define EXO_IO_TIMEOUT_MS       10000

static int g_listen_fd = -1;
static int g_listen_port = 0;

/* ============================================================================
 * Сериализация заголовка (network byte order, фиксированные 48 байт)
 * ============================================================================ */

static void put_u16(uint8_t **p, uint16_t v)
{
    (*p)[0] = (uint8_t)(v >> 8);
    (*p)[1] = (uint8_t)(v);
    *p += 2;
}

static void put_u32(uint8_t **p, uint32_t v)
{
    (*p)[0] = (uint8_t)(v >> 24);
    (*p)[1] = (uint8_t)(v >> 16);
    (*p)[2] = (uint8_t)(v >> 8);
    (*p)[3] = (uint8_t)(v);
    *p += 4;
}

static void put_u64(uint8_t **p, uint64_t v)
{
    put_u32(p, (uint32_t)(v >> 32));
    put_u32(p, (uint32_t)v);
}

static uint16_t get_u16(const uint8_t **p)
{
    uint16_t v = (uint16_t)(((*p)[0] << 8) | (*p)[1]);
    *p += 2;
    return v;
}

static uint32_t get_u32(const uint8_t **p)
{
    uint32_t v = ((uint32_t)(*p)[0] << 24) | ((uint32_t)(*p)[1] << 16) |
                 ((uint32_t)(*p)[2] << 8)  | (uint32_t)(*p)[3];
    *p += 4;
    return v;
}

static uint64_t get_u64(const uint8_t **p)
{
    uint64_t hi = get_u32(p);
    uint64_t lo = get_u32(p);
    return (hi << 32) | lo;
}

static size_t hdr_serialize(const exo_tensor_msg_t *hdr, uint8_t out[48])
{
    uint8_t *p = out;
    put_u32(&p, hdr->magic);
    put_u16(&p, hdr->version);
    put_u16(&p, hdr->msg_type);
    put_u32(&p, hdr->seq);
    put_u64(&p, hdr->request_id);
    put_u32(&p, hdr->dtype);
    put_u32(&p, hdr->n_dims);
    for (int i = 0; i < EXO_TENSOR_MAX_DIMS; i++)
        put_u32(&p, hdr->dims[i]);
    put_u32(&p, hdr->payload_len);
    return (size_t)(p - out);  /* == 48 */
}

static int hdr_deserialize(const uint8_t in[48], exo_tensor_msg_t *hdr)
{
    const uint8_t *p = in;
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = get_u32(&p);
    hdr->version = get_u16(&p);
    hdr->msg_type = get_u16(&p);
    hdr->seq = get_u32(&p);
    hdr->request_id = get_u64(&p);
    hdr->dtype = get_u32(&p);
    hdr->n_dims = get_u32(&p);
    for (int i = 0; i < EXO_TENSOR_MAX_DIMS; i++)
        hdr->dims[i] = get_u32(&p);
    hdr->payload_len = get_u32(&p);

    if (hdr->magic != EXO_TENSOR_MAGIC ||
        hdr->version != EXO_PROTO_VERSION ||
        hdr->n_dims > EXO_TENSOR_MAX_DIMS)
        return EXO_ERR_PROTOCOL;
    return EXO_OK;
}

/* ============================================================================
 * Низкоуровневые helpers с таймаутами и polling
 * ============================================================================ */

/* Прочитать ровно len байт из сокета (неблокирующий socket_recv + poll). */
static int tcp_read_exact(int fd, uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    size_t got = 0;
    uint64_t start = hal_timer_get_milliseconds();

    while (got < len) {
        int n = socket_recv(fd, buf + got, len - got);
        if (n > 0) {
            got += (size_t)n;
            continue;
        }
        if (timeout_ms &&
            hal_timer_get_milliseconds() - start > timeout_ms)
            return EXO_ERR_TIMEOUT;
        tcpip_poll();
    }
    return EXO_OK;
}

/* Записать весь буфер чанками <= EXO_TCP_CHUNK. */
static int tcp_write_all(int fd, const uint8_t *buf, size_t len,
                         uint32_t timeout_ms)
{
    size_t sent = 0;
    uint64_t start = hal_timer_get_milliseconds();

    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > EXO_TCP_CHUNK) chunk = EXO_TCP_CHUNK;

        int ret = socket_send(fd, buf + sent, chunk);
        if (ret >= 0) {
            sent += chunk;
            /* Дать стеку обработать ACK/окно и дренировать rx у пира */
            tcpip_poll();
            continue;
        }
        if (ret == NET_ERR_UNREACHABLE) {
            /* ARP ещё не разрешён — подождать ответ */
            if (timeout_ms &&
                hal_timer_get_milliseconds() - start > timeout_ms)
                return EXO_ERR_TIMEOUT;
            tcpip_poll();
            continue;
        }
        return EXO_ERR_NET;
    }
    return EXO_OK;
}

/* Дождаться установления TCP-соединения (SYN handshake идёт в tcpip_poll). */
static int tcp_wait_established(int fd, uint32_t timeout_ms)
{
    uint64_t start = hal_timer_get_milliseconds();

    for (;;) {
        /* TODO(tcpip.c): заменить на публичный socket_state(fd), когда появится */
        socket_t *s = tcpip_get_socket_for_testing(fd);
        if (!s || !s->active)
            return EXO_ERR_NET;
        if (s->state == TCP_ESTABLISHED)
            return EXO_OK;
        if (s->state == TCP_CLOSED)
            return EXO_ERR_NET;
        if (timeout_ms &&
            hal_timer_get_milliseconds() - start > timeout_ms)
            return EXO_ERR_TIMEOUT;
        tcpip_poll();
    }
}

/* ============================================================================
 * Публичный API
 * ============================================================================ */

int exo_transport_listen(uint16_t port)
{
    if (g_listen_fd >= 0) return EXO_OK;

    int fd = socket_create(SOCK_STREAM, IP_PROTO_TCP);
    if (fd < 0) return EXO_ERR_NET;
    if (socket_bind(fd, 0, port) != NET_OK ||
        socket_listen(fd, 1) != NET_OK) {
        socket_close(fd);
        return EXO_ERR_NET;
    }

    g_listen_fd = fd;
    g_listen_port = port;
    console_printf("[EXO] tensor transport listening on tcp :%d\n", port);
    return EXO_OK;
}

int exo_send_tensor(uint32_t dst_ip, uint16_t dst_port,
                    const exo_tensor_msg_t *hdr, const void *payload)
{
    if (!hdr || hdr->magic != EXO_TENSOR_MAGIC)
        return EXO_ERR_PROTOCOL;
    if (hdr->payload_len > 0 && !payload)
        return EXO_ERR_PROTOCOL;

    int fd = socket_create(SOCK_STREAM, IP_PROTO_TCP);
    if (fd < 0) return EXO_ERR_NET;

    int ret = socket_connect(fd, dst_ip, dst_port);
    if (ret != NET_OK) {
        socket_close(fd);
        return EXO_ERR_NET;
    }
    ret = tcp_wait_established(fd, EXO_CONNECT_TIMEOUT_MS);
    if (ret != EXO_OK) {
        socket_close(fd);
        return ret;
    }

    uint8_t hdr_buf[48];
    size_t hdr_len = hdr_serialize(hdr, hdr_buf);

    ret = tcp_write_all(fd, hdr_buf, hdr_len, EXO_IO_TIMEOUT_MS);
    if (ret == EXO_OK && hdr->payload_len > 0)
        ret = tcp_write_all(fd, (const uint8_t *)payload,
                            hdr->payload_len, EXO_IO_TIMEOUT_MS);

    socket_close(fd);
    return ret;
}

int exo_recv_tensor(int fd, exo_tensor_msg_t *hdr,
                    void *payload, size_t payload_cap, uint32_t timeout_ms)
{
    if (!hdr) return EXO_ERR_PROTOCOL;

    uint8_t hdr_buf[48];
    int ret = tcp_read_exact(fd, hdr_buf, sizeof(hdr_buf), timeout_ms);
    if (ret != EXO_OK) return ret;

    ret = hdr_deserialize(hdr_buf, hdr);
    if (ret != EXO_OK) {
        console_printf("[EXO] transport: bad message header (magic)\n");
        return ret;
    }

    if (hdr->payload_len == 0)
        return EXO_OK;
    if (!payload || hdr->payload_len > payload_cap)
        return EXO_ERR_PROTOCOL;

    return tcp_read_exact(fd, (uint8_t *)payload, hdr->payload_len, timeout_ms);
}

void exo_transport_poll(void)
{
    if (g_listen_fd < 0) return;

    /* TODO(tcpip.c): публичный способ узнать о входящем SYN без
     * tcpip_get_socket_for_testing() */
    socket_t *s = tcpip_get_socket_for_testing(g_listen_fd);
    if (!s || !s->active)
        return;
    /* SYN получен (SYN_RECEIVED) или handshake завершён (ESTABLISHED) */
    if (s->state != TCP_SYN_RECEIVED && s->state != TCP_ESTABLISHED)
        return;

    uint32_t remote_ip = 0;
    uint16_t remote_port = 0;
    int conn = socket_accept(g_listen_fd, &remote_ip, &remote_port);
    if (conn < 0) return;

    char ip_str[16];
    ip_to_string(remote_ip, ip_str, sizeof(ip_str));
    console_printf("[EXO] tensor connection from %s:%u\n",
                   ip_str, (unsigned)remote_port);

    /* Принять одно сообщение на этом соединении и передать оркестратору.
     * Максимальный payload — скрытый вектор модели (f32): 64 КБ с запасом. */
    static uint8_t payload_buf[64 * 1024];
    exo_tensor_msg_t hdr;

    int ret = exo_recv_tensor(conn, &hdr, payload_buf,
                              sizeof(payload_buf), EXO_IO_TIMEOUT_MS);
    if (ret == EXO_OK) {
        exo__handle_tensor_msg(conn, &hdr, payload_buf);
    } else {
        console_printf("[EXO] transport: recv failed (%d)\n", ret);
    }

    socket_close(conn);

    /* accept() переиспользовал listen-fd → пересоздать listener */
    g_listen_fd = -1;
    exo_transport_listen((uint16_t)g_listen_port);
}
