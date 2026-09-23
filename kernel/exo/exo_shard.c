/* EMBODIOS exo-style distributed inference — шардирование модели.
 *
 * Ring memory weighted partitioning — аналог
 * exo/topology/ring_memory_weighted_partitioning_strategy.py:
 *
 *  1. Ноды сортируются по свободной памяти (убывание) — это порядок кольца.
 *  2. Каждой ноде назначается доля [start,end) ∈ [0,1), равная
 *     ram_free_i / sum(ram_free).
 *  3. Доля мапится в непрерывный диапазон слоёв [start_layer, end_layer).
 *  4. Кольцо замыкается: следующая нода = (ring_index + 1) % ring_size.
 *     Нода с ring_index 0 владеет embedding (слой 0), последняя —
 *     output_norm/lm_head и sampling.
 *
 * Целочисленная арифметика (float в ядре не используем для партиций).
 */

#include <embodios/exo.h>
#include <embodios/console.h>
#include <embodios/streaming_inference.h>
#include <embodios/gguf_parser.h>
#include <string.h>

#include "exo_internal.h"

/* ============================================================================
 * Состояние
 * ============================================================================ */

static exo_shard_t g_local_shard;
static bool        g_shard_valid = false;
static bool        g_ring_degraded = false;  /* сбой TENSOR/RESULT в генерации */

/* Сохранённые параметры для rebalance */
static char     g_model_id[EXO_MODEL_ID_LEN];
static uint32_t g_n_layers = 0;
static exo_shard_strategy_t g_strategy = EXO_STRATEGY_RING_MEMORY_WEIGHTED;

/* Порядок кольца: индексы нод (в терминах exo_node_get()), отсортированные
 * по ram_free (убывание). */
static int      g_ring_order[EXO_MAX_NODES];
static int      g_ring_size = 0;

/* ============================================================================
 * Построение кольца
 * ============================================================================ */

/* Компаратор порядка кольца: ram_free по убыванию; при равенстве —
 * ДЕТЕРМИНИРОВАННЫЙ tiebreak по node_id (лексикографически, strncmp).
 * Таблица discovery на разных нодах заполняется в порядке прихода анонсов,
 * поэтому без tiebreak'а ноды с равной RAM строили бы РАЗНЫЕ кольца. */
static bool ring_node_before(const exo_node_t *a, const exo_node_t *b)
{
    uint64_t ra = a ? a->ram_free : 0;
    uint64_t rb = b ? b->ram_free : 0;
    if (ra != rb) return ra > rb;
    const char *ia = a ? a->node_id : "";
    const char *ib = b ? b->node_id : "";
    return strncmp(ia, ib, EXO_NODE_ID_LEN) < 0;
}

/* Сняпшот активных нод + сортировка по ring_node_before
 * (insertion sort — нод мало). Порядок полностью определяется содержимым
 * таблицы (ram_free, node_id), не порядком слотов → одинаков на всех нодах. */
static int ring_build(const exo_node_t *nodes_out[EXO_MAX_NODES])
{
    int n = exo_node_count();
    if (n <= 0) return 0;
    if (n > EXO_MAX_NODES) n = EXO_MAX_NODES;

    for (int i = 0; i < n; i++) {
        g_ring_order[i] = i;
        nodes_out[i] = exo_node_get(i);
    }

    for (int i = 1; i < n; i++) {
        int key = g_ring_order[i];
        int j = i - 1;
        while (j >= 0 &&
               ring_node_before(nodes_out[key], nodes_out[g_ring_order[j]])) {
            g_ring_order[j + 1] = g_ring_order[j];
            j--;
        }
        g_ring_order[j + 1] = key;
    }
    return n;
}

/* ============================================================================
 * Публичный API
 * ============================================================================ */

int exo_shard_assign(const char *model_id, uint32_t n_layers,
                     exo_shard_strategy_t strategy)
{
    if (!model_id || n_layers == 0)
        return EXO_ERR_PROTOCOL;

    strncpy(g_model_id, model_id, EXO_MODEL_ID_LEN - 1);
    g_model_id[EXO_MODEL_ID_LEN - 1] = '\0';
    g_n_layers = n_layers;
    g_strategy = strategy;

    const exo_node_t *nodes[EXO_MAX_NODES];
    int n = ring_build(nodes);
    if (n == 0)
        return EXO_ERR_NOSHARD;

    g_ring_size = n;

    /* Суммарная память для weighted partition */
    uint64_t total_ram = 0;
    for (int i = 0; i < n; i++)
        total_ram += nodes[g_ring_order[i]]->ram_free;
    if (total_ram == 0) {
        /* Память неизвестна (нет tcpip-геттера/анонсов) — fallback: поровну */
        strategy = EXO_STRATEGY_RING_EVEN;
    }

    /* Расчёт диапазонов слоёв */
    uint32_t layer_cursor = 0;
    int self_ring_index = -1;
    exo_shard_t self_shard;
    memset(&self_shard, 0, sizeof(self_shard));

    for (int i = 0; i < n; i++) {
        const exo_node_t *node = nodes[g_ring_order[i]];

        uint32_t count;
        if (i == n - 1) {
            count = n_layers - layer_cursor;  /* последний забирает остаток */
        } else if (strategy == EXO_STRATEGY_RING_EVEN) {
            count = n_layers / (uint32_t)n;
        } else {
            /* layers_i = round(n_layers * ram_i / total_ram) */
            uint64_t num = (uint64_t)n_layers * node->ram_free
                         + total_ram / 2;
            count = (uint32_t)(num / total_ram);
            if (count == 0) count = 1;             /* минимум слой на ноду */
            if (layer_cursor + count > n_layers)   /* страховка от переполнения */
                count = n_layers - layer_cursor;
        }

        if (node->is_local) {
            strncpy(self_shard.model_id, model_id, EXO_MODEL_ID_LEN - 1);
            self_shard.start_layer = layer_cursor;
            self_shard.end_layer = layer_cursor + count;
            self_shard.n_layers = n_layers;
            self_shard.ring_index = (uint32_t)i;
            self_shard.ring_size = (uint32_t)n;
            self_ring_index = i;
        }

        console_printf("[EXO] ring[%d] node '%s': layers %u..%u (%u)\n",
                       i, node->node_id, layer_cursor,
                       layer_cursor + count, count);
        layer_cursor += count;
    }

    if (self_ring_index < 0) {
        console_printf("[EXO] shard assign: local node not in table\n");
        return EXO_ERR_NOSHARD;
    }

    g_local_shard = self_shard;
    g_shard_valid = true;
    g_ring_degraded = false;  /* новое кольцо собрано — сбросить деградацию */

    console_printf("[EXO] local shard: model '%s' layers %u..%u of %u, "
                   "ring %u/%u\n",
                   g_local_shard.model_id, g_local_shard.start_layer,
                   g_local_shard.end_layer, g_local_shard.n_layers,
                   g_local_shard.ring_index, g_local_shard.ring_size);
    return EXO_OK;
}

const exo_shard_t* exo_shard_local(void)
{
    return g_shard_valid ? &g_local_shard : NULL;
}

/* Отложенный rebalance: join/leave во время кольцевой генерации */
static bool g_rebalance_pending = false;

int exo_shard_rebalance(void)
{
    if (!g_shard_valid || g_n_layers == 0)
        return EXO_ERR_NOSHARD;  /* ещё не было первичного назначения */

    if (exo_ring_busy()) {
        /* Генерация идёт — смена шардов посреди прохода рассинхронизирует
         * кольцо; отложить до конца генерации (exo_shard_check_pending). */
        g_rebalance_pending = true;
        console_printf("[EXO] ring busy — rebalance deferred\n");
        return EXO_OK;
    }

    console_printf("[EXO] rebalancing ring (model '%s', %u layers)\n",
                   g_model_id, g_n_layers);

    /* TODO(orchestrator): сбросить незавершённые request_id и KV-cache
     * перед сменой шарда — см. exo_node.c (request table). */
    return exo_shard_assign(g_model_id, g_n_layers, g_strategy);
}

void exo_shard_check_pending(void)
{
    if (g_rebalance_pending && !exo_ring_busy()) {
        g_rebalance_pending = false;
        console_printf("[EXO] applying deferred rebalance\n");
        exo_shard_rebalance();
    }
}

int exo_ring_next(int node_idx)
{
    if (!g_shard_valid || g_ring_size == 0)
        return -1;

    /* Найти node_idx в кольцевом порядке и вернуть следующего */
    for (int i = 0; i < g_ring_size; i++) {
        if (g_ring_order[i] == node_idx)
            return g_ring_order[(i + 1) % g_ring_size];
    }
    return -1;
}

/* ============================================================================
 * Auto ring / auto shard (v0.7.0) + состояние деградации
 * ============================================================================ */

void exo_ring_mark_degraded(const char *reason)
{
    if (!g_ring_degraded) {
        g_ring_degraded = true;
        console_printf("[EXO] ring DEGRADED: %s "
                       "(rejoin peer or re-run 'exoshard')\n",
                       reason ? reason : "transport failure");
    }
}

bool exo_ring_is_degraded(void)
{
    return g_ring_degraded;
}

int exo_ring_auto(void)
{
    const exo_node_t *nodes[EXO_MAX_NODES];
    int n = ring_build(nodes);
    if (n == 0) {
        console_printf("[EXO] exoring auto: discovery table empty "
                       "(is exo running?)\n");
        return EXO_ERR_NOSHARD;
    }

    /* Сохранить построенный порядок даже без назначенного шарда:
     * g_ring_order уже обновлён ring_build, g_ring_size выставляется
     * только при assign (exo_ring_next без шарда не используется). */
    console_printf("\nexo auto ring: %d node(s)%s\n", n,
                   g_ring_degraded ? "  [DEGRADED]" : "");
    for (int i = 0; i < n; i++) {
        const exo_node_t *nd = nodes[g_ring_order[i]];
        char ip[16];
        ip_to_string(nd->ip, ip, sizeof(ip));
        console_printf(" ring[%d] %-16s %s:%u  ram_free=%u MB%s%s\n",
                       i, nd->node_id, ip, (unsigned)nd->ctrl_port,
                       (unsigned)(nd->ram_free >> 20),
                       i == 0 ? "  <orchestrator>" : "",
                       nd->is_local ? " (local)" : "");
    }
    console_printf(" ring closes: ring[%d] -> ring[0] ('%s')\n\n",
                   n - 1, nodes[g_ring_order[0]]->node_id);

    /* Если шард уже назначен — пересобрать кольцо по свежей таблице
     * (join/leave пиров), сохранив модель/число слоёв/стратегию. */
    if (g_shard_valid && g_n_layers > 0) {
        console_printf("[EXO] re-applying shard to fresh ring "
                       "(model '%s', %u layers)\n", g_model_id, g_n_layers);
        return exo_shard_assign(g_model_id, g_n_layers, g_strategy);
    }
    return EXO_OK;
}

int exo_shard_auto(void)
{
    char model[EXO_MODEL_ID_LEN];
    model[0] = '\0';

    /* model_id: из предыдущего назначения, иначе из загруженной GGUF */
    if (g_model_id[0]) {
        strncpy(model, g_model_id, sizeof(model) - 1);
    } else {
        extern const char *gguf_get_model_name(void);
        const char *mn = gguf_get_model_name();
        if (mn && mn[0] && strncmp(mn, "unknown", 7) != 0)
            strncpy(model, mn, sizeof(model) - 1);
    }

    /* n_layers: из предыдущего назначения, иначе из движка/GGUF-метаданных */
    uint32_t n_layers = g_n_layers;
    if (n_layers == 0) {
        if (streaming_inference_is_ready()) {
            int layers = 0;
            streaming_inference_get_info(NULL, &layers, NULL, NULL);
            n_layers = (uint32_t)layers;
        } else {
            n_layers = GGUF_GET_N_LAYER();  /* GGUF распарсена, движок — нет */
        }
    }

    if (!model[0] || n_layers == 0) {
        console_printf("[EXO] exoshard auto: model unknown — load it first "
                       "(warm-up 'chat hi') or use "
                       "'exoshard <model> <n_layers>'\n");
        return EXO_ERR_NOSHARD;
    }

    console_printf("[EXO] exoshard auto: model '%s', %u layers, "
                   "RAM-weighted split\n", model, n_layers);
    return exo_shard_assign(model, n_layers, EXO_STRATEGY_RING_MEMORY_WEIGHTED);
}
