/* EMBODIOS exo-style distributed inference — UDP discovery.
 *
 * Аналог exo/networking/udp/udp_discovery.py (классическая exo):
 *  - каждая нода раз в EXO_ANNOUNCE_INTERVAL_MS (2.5 с) шлёт JSON-анонс
 *    UDP broadcast'ом на порт 5678;
 *  - слушатель принимает чужие анонсы, ведёт таблицу пиров
 *    с таймаутом EXO_NODE_TIMEOUT_MS (30 с).
 *
 * Реализация поверх net/tcpip.c:
 *  - приём: SOCK_DGRAM-сокет, socket_bind(port=5678) + socket_recv()
 *    (handle_udp в tcpip.c раскладывает датаграммы по local_port);
 *  - передача: tcpip_send_udp(0xFFFFFFFF, 5678, ...).
 *
 * Зависимости от net/tcpip.c (реализованы при интеграции):
 *  - TX broadcast: tcpip_send_udp()/tcp_send_packet() мапят
 *    255.255.255.255 и подсетевой broadcast (ip | ~netmask) на eth
 *    FF:FF:FF:FF:FF:FF без ARP (resolve_dst_mac()).
 *  - RX subnet broadcast: handle_ip() принимает точный IP, limited и
 *    subnet broadcast.
 *  - Геттеры конфигурации: tcpip_get_local_ip()/tcpip_get_local_mac()
 *    (net/tcpip.c), объявлены в embodios/tcpip.h.
 *  - Адрес отправителя датаграммы: socket_recvfrom().
 */

#include <embodios/exo.h>
#include <embodios/tcpip.h>
#include <embodios/mm.h>
#include <embodios/console.h>
#include <embodios/hal_timer.h>
#include <string.h>
#include <stdio.h>

#include "exo_internal.h"

/* ============================================================================
 * Таблица нод
 * ============================================================================ */

static exo_node_t g_nodes[EXO_MAX_NODES];
static int        g_discovery_fd = -1;
static uint64_t   g_last_announce_ms = 0;
static bool       g_started = false;

/* ============================================================================
 * Минимальный JSON: извлечение полей из анонса
 * (анонс компактный, генерируется нами же — достаточно наивного парсера)
 * ============================================================================ */

/* Найти "key":"value" и скопировать value. Возвращает 0 при успехе. */
static int json_get_str(const char *json, const char *key,
                        char *out, size_t out_cap)
{
    char pat[40];
    if (snprintf(pat, sizeof(pat), "\"%s\":\"", key) < 0)
        return -1;
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    const char *end = strchr(p, '"');
    if (!end) return -1;
    size_t n = (size_t)(end - p);
    if (n >= out_cap) n = out_cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 0;
}

/* Найти "key":<number> и вернуть значение. Возвращает 0 при успехе. */
static int json_get_u64(const char *json, const char *key, uint64_t *out)
{
    char pat[40];
    if (snprintf(pat, sizeof(pat), "\"%s\":", key) < 0)
        return -1;
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);

    uint64_t v = 0;
    bool any = false;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (uint64_t)(*p - '0');
        any = true;
        p++;
    }
    if (!any) return -1;
    *out = v;
    return 0;
}

/* ============================================================================
 * Таблица пиров
 * ============================================================================ */

static exo_node_t* node_table_find_slot(const char *node_id)
{
    int free_slot = -1;
    for (int i = 0; i < EXO_MAX_NODES; i++) {
        if (!g_nodes[i].active) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (strncmp(g_nodes[i].node_id, node_id, EXO_NODE_ID_LEN) == 0)
            return &g_nodes[i];
    }
    if (free_slot >= 0) return &g_nodes[free_slot];
    return NULL;
}

static void node_table_expire(void)
{
    uint64_t now = hal_timer_get_milliseconds();
    for (int i = 0; i < EXO_MAX_NODES; i++) {
        if (!g_nodes[i].active || g_nodes[i].is_local || g_nodes[i].pinned)
            continue;
        if (now - g_nodes[i].last_seen_ms > EXO_NODE_TIMEOUT_MS) {
            console_printf("[EXO] node '%s' timed out\n", g_nodes[i].node_id);
            g_nodes[i].active = false;
            /* join/leave меняет кольцо — перепартицирование */
            exo_shard_rebalance();
        }
    }
}

static void node_table_upsert(const char *node_id, uint32_t ip,
                              uint16_t ctrl_port, uint64_t ram_total,
                              uint64_t ram_free, const char *model_id)
{
    const exo_node_t *self = exo__self();
    if (self && strncmp(node_id, self->node_id, EXO_NODE_ID_LEN) == 0)
        return;  /* свой анонс (broadcast петля) — игнорируем */

    exo_node_t *n = node_table_find_slot(node_id);
    if (!n) {
        console_printf("[EXO] node table full, dropping '%s'\n", node_id);
        return;
    }

    bool is_new = !n->active;
    bool was_pinned = n->pinned;  /* pinned переживает refresh анонсом */
    memset(n, 0, sizeof(*n));
    n->pinned = was_pinned;
    strncpy(n->node_id, node_id, EXO_NODE_ID_LEN - 1);
    n->ip = ip;
    n->ctrl_port = ctrl_port;
    n->ram_total = ram_total;
    n->ram_free = ram_free;
    if (model_id)
        strncpy(n->model_id, model_id, EXO_MODEL_ID_LEN - 1);
    n->last_seen_ms = hal_timer_get_milliseconds();
    n->is_local = false;
    n->active = true;

    if (is_new) {
        char ip_str[16];
        ip_to_string(ip, ip_str, sizeof(ip_str));
        console_printf("[EXO] discovered node '%s' at %s:%u ram=%u MB\n",
                       n->node_id, ip_str, (unsigned)ctrl_port,
                       (unsigned)(ram_free >> 20));
        exo_shard_rebalance();
    }
}

/* ============================================================================
 * Публичный API
 * ============================================================================ */

int exo_discovery_start(void)
{
    if (g_started) return EXO_OK;

    memset(g_nodes, 0, sizeof(g_nodes));

    g_discovery_fd = socket_create(SOCK_DGRAM, IP_PROTO_UDP);
    if (g_discovery_fd < 0) {
        console_printf("[EXO] discovery: socket_create failed\n");
        return EXO_ERR_NET;
    }
    if (socket_bind(g_discovery_fd, 0, EXO_DISCOVERY_PORT) != NET_OK) {
        console_printf("[EXO] discovery: bind :%d failed\n", EXO_DISCOVERY_PORT);
        return EXO_ERR_NET;
    }

    /* Локальная нода — слот 0 таблицы */
    const exo_node_t *self = exo__self();
    if (self) {
        g_nodes[0] = *self;
        g_nodes[0].is_local = true;
        g_nodes[0].active = true;
        g_nodes[0].last_seen_ms = hal_timer_get_milliseconds();
    }

    g_last_announce_ms = 0;
    g_started = true;
    console_printf("[EXO] discovery started on udp :%d\n", EXO_DISCOVERY_PORT);
    return EXO_OK;
}

int exo_discovery_announce(void)
{
    const exo_node_t *self = exo__self();
    if (!self) return EXO_ERR_INIT;

    char json[256];
    int len = snprintf(json, sizeof(json),
        "{\"type\":\"discovery\",\"node_id\":\"%s\",\"ctrl_port\":%u,"
        "\"memory\":%u,\"memory_free\":%u,\"model\":\"%s\"}",
        self->node_id, (unsigned)self->ctrl_port,
        (unsigned)(self->ram_total >> 20),   /* MB — достаточно для partition */
        (unsigned)(self->ram_free >> 20),
        self->model_id);
    if (len <= 0) return EXO_ERR_PROTOCOL;

    /* TX broadcast: tcpip.c мапит 255.255.255.255 на eth FF:FF:FF:FF:FF:FF
     * без ARP (resolve_dst_mac) */
    int ret = tcpip_send_udp(0xFFFFFFFFu, EXO_DISCOVERY_PORT,
                             EXO_DISCOVERY_PORT, json, (size_t)len);
    return (ret >= 0) ? EXO_OK : EXO_ERR_NET;
}

void exo_discovery_poll(void)
{
    if (!g_started) return;

    uint64_t now = hal_timer_get_milliseconds();

    /* Периодический анонс */
    if (now - g_last_announce_ms >= EXO_ANNOUNCE_INTERVAL_MS) {
        g_last_announce_ms = now;
        exo_discovery_announce();

        /* Обновить ram_free локальной ноды для weighted partitioning */
        const exo_node_t *self = exo__self();
        if (self && g_nodes[0].is_local) {
            g_nodes[0].ram_free = self->ram_free;
            g_nodes[0].last_seen_ms = now;
        }
    }

    /* Приём чужих анонсов (неблокирующе), с адресом отправителя */
    uint8_t buf[1024];
    for (;;) {
        uint32_t src_ip = 0;
        uint16_t src_port = 0;
        int n = socket_recvfrom(g_discovery_fd, buf, sizeof(buf) - 1,
                                &src_ip, &src_port);
        if (n <= 0) break;
        buf[n] = '\0';

        char node_id[EXO_NODE_ID_LEN];
        char model_id[EXO_MODEL_ID_LEN];
        uint64_t ctrl_port = 0, mem = 0, mem_free = 0;

        if (!strstr((const char *)buf, "\"type\":\"discovery\""))
            continue;
        if (json_get_str((const char *)buf, "node_id",
                         node_id, sizeof(node_id)) != 0)
            continue;
        json_get_u64((const char *)buf, "ctrl_port", &ctrl_port);
        json_get_u64((const char *)buf, "memory", &mem);
        json_get_u64((const char *)buf, "memory_free", &mem_free);
        model_id[0] = '\0';
        json_get_str((const char *)buf, "model", model_id, sizeof(model_id));

        /* src_ip получен из socket_recvfrom() выше */
        node_table_upsert(node_id, src_ip, (uint16_t)ctrl_port,
                          mem << 20, mem_free << 20, model_id);
    }

    node_table_expire();
}

/* Статический пир (команда exopeer): для транспортов без broadcast
 * (QEMU user-net/slirp изолирует гостей; пир указывается вручную,
 * напр. через hostfwd: ip=10.0.2.2, port=хост-порт второй ноды).
 * Пир не истекает по EXO_NODE_TIMEOUT_MS. */
int exo_discovery_add_peer(const char *node_id, uint32_t ip,
                           uint16_t ctrl_port, uint64_t ram_mb)
{
    if (!g_started) return EXO_ERR_INIT;
    if (!node_id || !node_id[0] || !ip || !ctrl_port)
        return EXO_ERR_PROTOCOL;

    node_table_upsert(node_id, ip, ctrl_port,
                      ram_mb << 20, ram_mb << 20, "static");

    /* Пометить как pinned (upsert мог обновить существующего) */
    for (int i = 0; i < EXO_MAX_NODES; i++) {
        if (g_nodes[i].active &&
            strncmp(g_nodes[i].node_id, node_id, EXO_NODE_ID_LEN) == 0) {
            g_nodes[i].pinned = true;
            console_printf("[EXO] static peer '%s' pinned at %u.%u.%u.%u:%u ram=%u MB\n",
                           node_id,
                           (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
                           (unsigned)((ip >> 16) & 0xFF), (unsigned)(ip >> 24),
                           (unsigned)ctrl_port, (unsigned)ram_mb);
            return EXO_OK;
        }
    }
    return EXO_ERR_INIT;
}

int exo_node_count(void)
{
    int count = 0;
    for (int i = 0; i < EXO_MAX_NODES; i++)
        if (g_nodes[i].active) count++;
    return count;
}

const exo_node_t* exo_node_get(int index)
{
    int seen = 0;
    for (int i = 0; i < EXO_MAX_NODES; i++) {
        if (!g_nodes[i].active) continue;
        if (seen == index) return &g_nodes[i];
        seen++;
    }
    return NULL;
}

const exo_node_t* exo_node_find(const char *node_id)
{
    for (int i = 0; i < EXO_MAX_NODES; i++) {
        if (g_nodes[i].active &&
            strncmp(g_nodes[i].node_id, node_id, EXO_NODE_ID_LEN) == 0)
            return &g_nodes[i];
    }
    return NULL;
}
