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
/* Таймауты TCG-scaled: под 3-нодной TCG-контенцией гостевое время
 * растянуто; handshake и window-drain занимают секунды. */
#define EXO_CONNECT_TIMEOUT_MS  15000
#define EXO_IO_TIMEOUT_MS       30000

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
        if (ret == NET_ERR_UNREACHABLE || ret == -4 /*VIRTIO_ERR_TIMEOUT*/) {
            /* ARP ещё не разрешён, либо драйвер не дождался TX-completion
             * под нагрузкой — повторить до истечения таймаута. Повтор
             * безопасен: seq не инкрементируется при неудаче, а in-order
             * guard у получателя дропает дубликаты. */
            if (timeout_ms &&
                hal_timer_get_milliseconds() - start > timeout_ms)
                return EXO_ERR_TIMEOUT;
            tcpip_poll();
            continue;
        }
        console_printf("[EXO] transport: write fail fd=%d ret=%d state=%d\n",
                       fd, ret,
                       tcpip_get_socket_for_testing(fd)
                           ? tcpip_get_socket_for_testing(fd)->state : -99);
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
 * Персистентные соединения (v0.6.0)
 *
 * Раньше каждый hop открывал НОВОЕ TCP-соединение (connect-per-token):
 * SYN-штормы, TIME_WAIT-утечки, ephemeral-порты и accept-блокировки
 * делали кольцо нестабильным под TCG-нагрузкой (сбой после N токенов).
 * Теперь: одно исходящее соединение на (ip,port) переиспользуется между
 * токенами; входящие соединения живут постоянно и читаются асинхронно
 * (без блокировки главного цикла) с покадровым накоплением.
 * ============================================================================ */

#define EXO_OUT_CACHE   4
#define EXO_IN_MAX      6
#define EXO_IN_BUF      (48 + 16384)  /* hdr + hidden f32 до 4096 */

typedef struct {
    uint32_t ip;
    uint16_t port;
    int      fd;
} exo_out_conn_t;

typedef struct {
    int    fd;
    size_t have;
    uint8_t buf[EXO_IN_BUF];
} exo_in_conn_t;

static exo_out_conn_t g_out[EXO_OUT_CACHE];
static exo_in_conn_t  g_in[EXO_IN_MAX];

static void out_conn_drop(int slot)
{
    if (g_out[slot].fd >= 0) {
        socket_close(g_out[slot].fd);
        /* повторный close добивает FIN_WAIT/CLOSED-состояние */
        g_out[slot].fd = -1;
    }
}

/* Валидное открытое соединение до (ip,port) или -1. */
static int out_conn_get(uint32_t ip, uint16_t port)
{
    for (int i = 0; i < EXO_OUT_CACHE; i++) {
        if (g_out[i].fd < 0) continue;
        if (g_out[i].ip != ip || g_out[i].port != port) continue;
        socket_t *s = tcpip_get_socket_for_testing(g_out[i].fd);
        if (s && s->active && s->state == TCP_ESTABLISHED)
            return g_out[i].fd;
        out_conn_drop(i);  // stale
    }
    return -1;
}

/* Открыть новое соединение и положить в кэш. Возврат fd или <0. */
static int out_conn_new(uint32_t ip, uint16_t port, int *err_out)
{
    int slot = -1;
    for (int i = 0; i < EXO_OUT_CACHE; i++)
        if (g_out[i].fd < 0) { slot = i; break; }
    if (slot < 0) slot = 0;  /* вытеснение (старый fd уже мёртв) */

    int fd = socket_create(SOCK_STREAM, IP_PROTO_TCP);
    if (fd < 0) {
        console_printf("[EXO] transport: socket_create failed (table full)\n");
        if (err_out) *err_out = EXO_ERR_NET;
        return -1;
    }
    if (socket_connect(fd, ip, port) != NET_OK) {
        socket_close(fd);
        if (err_out) *err_out = EXO_ERR_NET;
        return -1;
    }
    int ret = tcp_wait_established(fd, EXO_CONNECT_TIMEOUT_MS);
    if (ret != EXO_OK) {
        console_printf("[EXO] transport: handshake timeout/fail (%d) to %u.%u.%u.%u:%u\n",
                       ret,
                       (unsigned)(ip >> 24) & 0xFF, (unsigned)(ip >> 16) & 0xFF,
                       (unsigned)(ip >> 8) & 0xFF, (unsigned)ip & 0xFF,
                       (unsigned)port);
        socket_close(fd);
        if (err_out) *err_out = ret;
        return -1;
    }
    out_conn_drop(slot);
    g_out[slot].ip = ip;
    g_out[slot].port = port;
    g_out[slot].fd = fd;
    return fd;
}

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

    uint8_t hdr_buf[48];
    size_t hdr_len = hdr_serialize(hdr, hdr_buf);

    /* Две попытки: первая на закэшированном соединении, вторая — на
     * свежем (пере-подключение после ошибки/разрыва). */
    int last_err = EXO_ERR_NET;
    for (int attempt = 0; attempt < 2; attempt++) {
        int fd = out_conn_get(dst_ip, dst_port);
        if (fd < 0) {
            int err = EXO_ERR_NET;
            fd = out_conn_new(dst_ip, dst_port, &err);
            if (fd < 0) { last_err = err; continue; }
        }

        int ret = tcp_write_all(fd, hdr_buf, hdr_len, EXO_IO_TIMEOUT_MS);
        if (ret == EXO_OK && hdr->payload_len > 0)
            ret = tcp_write_all(fd, (const uint8_t *)payload,
                                hdr->payload_len, EXO_IO_TIMEOUT_MS);
        if (ret == EXO_OK)
            return EXO_OK;

        /* Разрыв/ошибка: выкинуть из кэша и переподключиться один раз */
        last_err = ret;
        for (int i = 0; i < EXO_OUT_CACHE; i++)
            if (g_out[i].fd == fd) { out_conn_drop(i); break; }
    }
    return last_err;
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
        console_printf("[EXO] transport: bad message header (magic) fd=%d\n", fd);
        return ret;
    }

    if (hdr->payload_len == 0)
        return EXO_OK;
    if (!payload || hdr->payload_len > payload_cap)
        return EXO_ERR_PROTOCOL;

    return tcp_read_exact(fd, (uint8_t *)payload, hdr->payload_len, timeout_ms);
}

/* Асинхронный polling: accept новых + покадровое чтение постоянных
 * входящих соединений. БЕЗ блокировок — вызывается из главного цикла. */
void exo_transport_poll(void)
{
    /* accept: listen-сокет увидел SYN (SYN_RECEIVED/ESTABLISHED) */
    if (g_listen_fd >= 0) {
        socket_t *ls = tcpip_get_socket_for_testing(g_listen_fd);
        if (ls && ls->active &&
            (ls->state == TCP_SYN_RECEIVED || ls->state == TCP_ESTABLISHED)) {
            uint32_t remote_ip = 0;
            uint16_t remote_port = 0;
            int conn = socket_accept(g_listen_fd, &remote_ip, &remote_port);
            if (conn >= 0) {
                char ip_str[16];
                ip_to_string(remote_ip, ip_str, sizeof(ip_str));
                console_printf("[EXO] tensor connection from %s:%u\n",
                               ip_str, (unsigned)remote_port);
                int slot = -1;
                for (int i = 0; i < EXO_IN_MAX; i++)
                    if (g_in[i].fd < 0) { slot = i; break; }
                if (slot >= 0) {
                    g_in[slot].fd = conn;
                    g_in[slot].have = 0;
                } else {
                    console_printf("[EXO] transport: inbound table full\n");
                    socket_close(conn);
                }
                /* accept() переиспользовал listen-fd → пересоздать listener */
                g_listen_fd = -1;
                exo_transport_listen((uint16_t)g_listen_port);
            }
        }
    } else if (g_listen_port > 0) {
        /* listener умер (socket table churn) — восстановить */
        exo_transport_listen((uint16_t)g_listen_port);
    }

    /* сервис постоянных входящих соединений */
    for (int i = 0; i < EXO_IN_MAX; i++) {
        if (g_in[i].fd < 0) continue;
        int fd = g_in[i].fd;
        socket_t *s = tcpip_get_socket_for_testing(fd);
        if (!s || !s->active || s->state == TCP_CLOSED) {
            g_in[i].fd = -1;
            continue;
        }
        if (s->state == TCP_CLOSE_WAIT || s->state == TCP_LAST_ACK ||
            s->state == TCP_TIME_WAIT) {
            /* пир закрыл — добить и освободить слот */
            socket_close(fd);
            socket_t *s2 = tcpip_get_socket_for_testing(fd);
            if (s2 && s2->active && s2->state == TCP_LAST_ACK) {
                /* LAST_ACK ждёт финальный ACK пира; оставить стеку,
                 * слот переиспользуем только после фактического close */
                continue;
            }
            g_in[i].fd = -1;
            continue;
        }

        /* дренировать доступное в покадровый буфер */
        if (g_in[i].have < sizeof(g_in[i].buf)) {
            int n = socket_recv(fd, g_in[i].buf + g_in[i].have,
                                sizeof(g_in[i].buf) - g_in[i].have);
            if (n > 0)
                g_in[i].have += (size_t)n;
        }

        /* обработать все ПОЛНЫЕ сообщения в буфере */
        while (g_in[i].have >= 48) {
            exo_tensor_msg_t hdr;
            if (hdr_deserialize(g_in[i].buf, &hdr) != EXO_OK) {
                console_printf("[EXO] transport: bad magic fd=%d, closing\n", fd);
                socket_close(fd);
                g_in[i].fd = -1;
                break;
            }
            size_t need = 48 + (size_t)hdr.payload_len;
            if (need > sizeof(g_in[i].buf)) {
                console_printf("[EXO] transport: oversize payload %u, closing\n",
                               (unsigned)hdr.payload_len);
                socket_close(fd);
                g_in[i].fd = -1;
                break;
            }
            if (g_in[i].have < need)
                break;  /* ждать остаток payload в следующих poll */

            /* Трафик от пира = признак живости: во время кольцевой
             * генерации beacon'ы редки (блокирующий forward), поэтому
             * discovery-liveness подпитывается от тензорного трафика. */
            if (s->remote_ip)
                exo_discovery_touch_ip(s->remote_ip);

            exo__handle_tensor_msg(fd, &hdr, g_in[i].buf + 48);
            memmove(g_in[i].buf, g_in[i].buf + need, g_in[i].have - need);
            g_in[i].have -= need;
        }
    }
}
