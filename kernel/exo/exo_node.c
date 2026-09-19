/* EMBODIOS exo-style distributed inference — узел и оркестратор.
 *
 * Аналог exo/orchestration/node.py (классическая exo): состояние ноды,
 * инициализация подсистем, диспетчеризация входящих тензорных сообщений,
 * проход токена по кольцу:
 *
 *   PROMPT → нода с ring_index 0: embedding + свои слои → TENSOR следующей;
 *   TENSOR → свои слои [start,end) → TENSOR следующей;
 *   последняя нода (ring_index == ring_size-1): lm_head + sample →
 *   RESULT всем + embedding нового токена первой ноде (кольцо).
 *
 * KV-cache каждого слоя живёт локально на ноде-владельце; по сети идёт
 * только скрытый вектор (n_embd × dtype на токен на hop).
 *
 * Стыковка с ai/streaming_inference.c — через layer-range API:
 *   streaming_inference_embed()           — token_embd lookup (node ring[0])
 *   streaming_inference_forward_layers()  — прогон [start,end) с локальным KV
 *   streaming_inference_sample_token()    — final norm + lm_head + sampling
 *   streaming_inference_is_stop_token()   — EOS/stop проверка
 *
 * Протокол кольца (lockstep, по одному токену за проход):
 *   оркестратор (обязан быть ring[0]) для каждого токена: embed → forward
 *   своих слоёв → TENSOR следующей ноде; каждая нода: forward своих слоёв →
 *   TENSOR следующей; хвост кольца: forward → sample → RESULT оркестратору.
 *   RESULT — также барьер: оркестратор ждёт RESULT(seq) перед следующим
 *   токеном (транспорт tcpip.c держит одно соединение на listener, поэтому
 *   пайплайнинг prefill намеренно НЕ используется).
 *
 * TODO(integration):
 *  1. Шард-загрузчик GGUF: мапить только blk.[start..end) вместо всех слоёв
 *     (сейчас каждая нода держит модель целиком — работает, но не экономит
 *     память; см. MAP_LAYER_TENSOR, ai/streaming_inference.c).
 *  2. Оркестратор вне ring[0]: relay PROMPT к ring[0] и маршрутизация
 *     RESULT обратно на HTTP-ноду (сейчас — fallback на полный локальный
 *     прогон, модель замаплена целиком, результат корректный).
 *  3. Dtype f16 на проводе (сейчас только f32) и пайплайнинг prefill
 *     (нужны listener с несколькими соединениями и reassembly в tcpip.c).
 */

#include <embodios/exo.h>
#include <embodios/tcpip.h>
#include <embodios/mm.h>
#include <embodios/console.h>
#include <embodios/hal_timer.h>
#include <embodios/streaming_inference.h>
#include <embodios/bpe_tokenizer.h>
#include <embodios/chat_template.h>
#include <string.h>
#include <stdio.h>

#include "exo_internal.h"

/* ============================================================================
 * Состояние ноды
 * ============================================================================ */

static exo_node_t g_self;
static bool       g_running = false;
static uint64_t   g_request_seq = 0;

/* ============================================================================
 * Идентичность
 * ============================================================================ */

static void self_init_defaults(const char *node_id, uint16_t listen_port)
{
    memset(&g_self, 0, sizeof(g_self));

    if (node_id && node_id[0]) {
        strncpy(g_self.node_id, node_id, EXO_NODE_ID_LEN - 1);
    } else {
        /* По умолчанию — из MAC (если tcpip даст), иначе константа */
        uint8_t mac[6] = {0};
        if (tcpip_get_local_mac(mac) == NET_OK) {
            snprintf(g_self.node_id, sizeof(g_self.node_id),
                     "embodios-%02x%02x%02x", mac[3], mac[4], mac[5]);
        } else {
            strncpy(g_self.node_id, "embodios-node", EXO_NODE_ID_LEN - 1);
        }
    }

    g_self.ctrl_port = listen_port ? listen_port : EXO_DEFAULT_CTRL_PORT;

    uint32_t ip = 0;
    if (tcpip_get_local_ip(&ip) == NET_OK)
        g_self.ip = ip;

    g_self.ram_total = pmm_total_memory();
    g_self.ram_free  = pmm_available_memory();

    /* model_id из загруженной GGUF (general.name), если модель уже
     * загружена; иначе "unknown" (обновляется при чтении через
     * gguf_get_model_name() в exo_server) */
    {
        extern const char *gguf_get_model_name(void);
        const char *mn = gguf_get_model_name();
        strncpy(g_self.model_id, (mn && mn[0]) ? mn : "unknown",
                EXO_MODEL_ID_LEN - 1);
        g_self.model_id[EXO_MODEL_ID_LEN - 1] = '\0';
    }

    g_self.flops_fp16 = 0;  /* TODO: оценка по частоте TSC/бенчмарку matmul */
    g_self.is_local = true;
    g_self.active = true;
    g_self.last_seen_ms = hal_timer_get_milliseconds();
}

/* ============================================================================
 * Публичный API жизненного цикла
 * ============================================================================ */

int exo_init(const char *node_id, uint16_t listen_port)
{
    if (g_running) return EXO_OK;

    self_init_defaults(node_id, listen_port);

    char ip_str[16] = "?.?.?.?";
    if (g_self.ip) ip_to_string(g_self.ip, ip_str, sizeof(ip_str));
    console_printf("[EXO] node '%s' ip=%s ctrl=:%u ram=%u/%u MB\n",
                   g_self.node_id, ip_str, (unsigned)g_self.ctrl_port,
                   (unsigned)(g_self.ram_free >> 20),
                   (unsigned)(g_self.ram_total >> 20));

    int ret = exo_discovery_start();
    if (ret != EXO_OK) return ret;

    ret = exo_transport_listen(g_self.ctrl_port);
    if (ret != EXO_OK) return ret;

    g_running = true;
    return EXO_OK;
}

void exo_shutdown(void)
{
    if (!g_running) return;
    /* Сокеты discovery/transport закроются по таймауту tcpip;
     * явного teardown API у tcpip.c для listen-сокетов нет — TODO. */
    g_running = false;
    console_printf("[EXO] node '%s' stopped\n", g_self.node_id);
}

void exo_poll(void)
{
    if (!g_running) return;
    tcpip_poll();
    exo_discovery_poll();
    exo_transport_poll();
    exo_server_poll();
}

bool exo_is_running(void)
{
    return g_running;
}

const exo_node_t* exo__self(void)
{
    return g_self.active ? &g_self : NULL;
}

uint64_t exo__next_request_id(void)
{
    return ++g_request_seq;
}

/* ============================================================================
 * Прогон локальных слоёв
 * ============================================================================ */

int exo_forward_shard(const float *hidden_in, size_t len,
                      float *hidden_out, uint32_t pos)
{
    const exo_shard_t *shard = exo_shard_local();
    if (!shard) return EXO_ERR_NOSHARD;
    if (!hidden_in || !hidden_out || len == 0)
        return EXO_ERR_PROTOCOL;

    /* Модель должна быть загружена (embedded GGUF + streaming init) */
    if (!streaming_inference_is_ready()) {
        console_printf("[EXO] forward_shard: inference engine not ready\n");
        return EXO_ERR_UNSUPPORTED;
    }

    /* Длина скрытого вектора обязана совпадать с n_embd модели */
    int dim = 0;
    streaming_inference_get_info(&dim, NULL, NULL, NULL);
    if (dim <= 0 || len != (size_t)dim) {
        console_printf("[EXO] forward_shard: len=%u != model dim=%d\n",
                       (unsigned)len, dim);
        return EXO_ERR_PROTOCOL;
    }

    if (hidden_out != hidden_in)
        memcpy(hidden_out, hidden_in, len * sizeof(float));

    /* Прогон локального диапазона слоёв; KV-cache слоёв шарда — локальный
     * (streaming_inference_forward_layers индексирует KV по абсолютному
     * номеру слоя, чужие регионы не трогаются). */
    int ret = streaming_inference_forward_layers(hidden_out, (int)pos,
                                                 (int)shard->start_layer,
                                                 (int)shard->end_layer);
    console_printf("[EXO] forward_shard: layers %u..%u pos=%u len=%u -> %d\n",
                   shard->start_layer, shard->end_layer, pos,
                   (unsigned)len, ret);
    return (ret == 0) ? EXO_OK : EXO_ERR_UNSUPPORTED;
}

/* ============================================================================
 * Оркестратор: состояние активного кольцевого запроса
 * ============================================================================ */

/* Скрытый вектор не больше 16384 float (хватит на 32B-модели: n_embd 6144) */
#define EXO_MAX_HIDDEN          16384

/* Лимит токенов ответа по умолчанию для API-запросов без max_tokens
 * (под TCG-эмуляцией ~7 с/токен; OpenAI-клиент может задать max_tokens) */
#define EXO_DEFAULT_MAX_TOKENS  64

/* Таймаут ожидания RESULT от хвоста кольца: под TCG-эмуляцией прогон
 * половины SmolLM-135M + lm_head + sampling занимает десятки секунд. */
#define EXO_RING_RESULT_TIMEOUT_MS  180000

/* Активный запрос оркестратора (один за раз — lockstep протокол) */
typedef struct {
    bool     active;
    uint64_t request_id;
    bool     result_ready;      /* пришёл RESULT для request_id */
    uint32_t result_seq;        /* seq из последнего RESULT */
    int32_t  result_token;      /* сэмплированный хвостом токен */
    bool     result_finished;   /* хвост сигналит EOS/stop */
} exo_ring_req_t;

static exo_ring_req_t g_ring_req;

/* Индекс локальной ноды в таблице discovery (для exo_ring_next) */
static int self_table_index(void)
{
    for (int i = 0; i < exo_node_count(); i++) {
        const exo_node_t *n = exo_node_get(i);
        if (n && n->is_local) return i;
    }
    return -1;
}

/* Доставить RESULT локальному оркестратору (или залогировать чужой). */
static void deliver_result(const exo_tensor_msg_t *hdr, const uint8_t *payload)
{
    int32_t token = -1;
    if (payload && hdr->payload_len >= sizeof(int32_t))
        memcpy(&token, payload, sizeof(token));

    bool finished = (hdr->n_dims >= 2 && hdr->dims[1] != 0);

    console_printf("[EXO] RESULT req=%llu seq=%u token=%d finished=%d\n",
                   (unsigned long long)hdr->request_id, hdr->seq,
                   (int)token, finished ? 1 : 0);

    if (g_ring_req.active && hdr->request_id == g_ring_req.request_id) {
        g_ring_req.result_token    = token;
        g_ring_req.result_seq      = hdr->seq;
        g_ring_req.result_finished = finished;
        g_ring_req.result_ready    = true;
    }
}

/* Отправить RESULT ноде-оркестратору (ring head = следующая от хвоста). */
static int send_result_to_head(const exo_tensor_msg_t *tensor_hdr,
                               int32_t token, bool finished)
{
    int self_idx = self_table_index();
    int head_idx = exo_ring_next(self_idx);  /* кольцо: next(хвост) = ring[0] */
    if (head_idx < 0) return EXO_ERR_NOSHARD;
    const exo_node_t *head = exo_node_get(head_idx);
    if (!head) return EXO_ERR_NOSHARD;

    exo_tensor_msg_t out = {
        .magic = EXO_TENSOR_MAGIC,
        .version = EXO_PROTO_VERSION,
        .msg_type = EXO_MSG_RESULT,
        .seq = tensor_hdr->seq,
        .request_id = tensor_hdr->request_id,
        .dtype = EXO_DTYPE_I32,
        .n_dims = 2,
        .dims = { 1, finished ? 1u : 0u, 0, 0 },
        .payload_len = sizeof(int32_t),
    };

    if (head->is_local) {
        /* Оркестратор — мы сами (однонодное кольцо): доставить локально */
        uint8_t pl[sizeof(int32_t)];
        memcpy(pl, &token, sizeof(token));
        deliver_result(&out, pl);
        return EXO_OK;
    }
    return exo_send_tensor(head->ip, head->ctrl_port, &out, &token);
}

/* ============================================================================
 * Оркестратор: входящие сообщения кольца
 * ============================================================================ */

static void handle_tensor(const exo_tensor_msg_t *hdr, const uint8_t *payload)
{
    const exo_shard_t *shard = exo_shard_local();
    if (!shard) {
        console_printf("[EXO] TENSOR dropped: no shard assigned\n");
        return;
    }
    if (hdr->dtype != EXO_DTYPE_F32) {
        console_printf("[EXO] TENSOR dropped: dtype %u unsupported (f32 only)\n",
                       hdr->dtype);
        return;
    }

    /* Элементов в скрытом векторе */
    size_t n_elems = 1;
    for (uint32_t i = 0; i < hdr->n_dims && i < EXO_TENSOR_MAX_DIMS; i++)
        n_elems *= hdr->dims[i];

    static float hidden[EXO_MAX_HIDDEN];
    if (n_elems == 0 || n_elems > EXO_MAX_HIDDEN ||
        hdr->payload_len != n_elems * sizeof(float)) {
        console_printf("[EXO] TENSOR malformed: %u elems, payload %u\n",
                       (unsigned)n_elems, hdr->payload_len);
        return;
    }
    memcpy(hidden, payload, n_elems * sizeof(float));

    /* Прогон локальных слоёв; pos = seq (порядок токенов в запросе) */
    if (exo_forward_shard(hidden, n_elems, hidden, hdr->seq) != EXO_OK)
        return;

    bool last_in_ring = (shard->ring_index == shard->ring_size - 1);
    if (last_in_ring) {
        /* Хвост кольца: final norm + lm_head + sampling, RESULT оркестратору.
         * Токен сэмплирован всегда (в prefill оркестратор его игнорирует,
         * RESULT служит барьером lockstep-протокола). */
        int token = streaming_inference_sample_token(hidden);
        if (token < 0) {
            console_printf("[EXO] ring tail: sampling failed\n");
            return;
        }
        bool finished = streaming_inference_is_stop_token(token);
        int ret = send_result_to_head(hdr, (int32_t)token, finished);
        if (ret != EXO_OK)
            console_printf("[EXO] ring tail: RESULT send failed (%d)\n", ret);
        return;
    }

    /* Передать скрытый вектор следующей ноде кольца */
    int self_idx = self_table_index();
    int next_idx = exo_ring_next(self_idx);
    if (next_idx < 0) {
        console_printf("[EXO] ring: no next node\n");
        return;
    }
    const exo_node_t *next = exo_node_get(next_idx);
    if (!next) return;

    exo_tensor_msg_t out = *hdr;
    out.payload_len = (uint32_t)(n_elems * sizeof(float));
    int ret = exo_send_tensor(next->ip, next->ctrl_port, &out, hidden);
    if (ret != EXO_OK)
        console_printf("[EXO] forward to '%s' failed (%d)\n",
                       next->node_id, ret);
}

void exo__handle_tensor_msg(int fd, const exo_tensor_msg_t *hdr,
                            const uint8_t *payload)
{
    (void)fd;
    switch ((exo_msg_type_t)hdr->msg_type) {
    case EXO_MSG_HEALTH:
        /* Health-check: приём сам по себе — подтверждение живости */
        break;
    case EXO_MSG_TENSOR:
        handle_tensor(hdr, payload);
        break;
    case EXO_MSG_PROMPT:
        /* TODO(relay): запуск кольца удалённым оркестратором. В текущем
         * дизайне оркестратор обязан быть ring[0] и запускает проход сам
         * (exo_ring_generate), поэтому сетевой PROMPT не используется. */
        console_printf("[EXO] PROMPT req=%llu (%u bytes) — unsupported "
                       "(orchestrator must be ring[0])\n",
                       (unsigned long long)hdr->request_id,
                       hdr->payload_len);
        break;
    case EXO_MSG_RESULT:
        /* Токен, сэмплированный хвостом кольца */
        deliver_result(hdr, payload);
        break;
    case EXO_MSG_TOPOLOGY:
        /* TODO(gossip): отдать свою таблицу пиров (CollectTopology) */
        break;
    default:
        console_printf("[EXO] unknown msg type %u\n", hdr->msg_type);
        break;
    }
}

/* ============================================================================
 * Генерация: локальная (одна нода / fallback) и кольцевая (оркестратор)
 * ============================================================================
 * Оба пути собраны из layer-range API streaming_inference.c:
 *   embed → forward_layers(shard) → [кольцо | sample].
 * Одна нода = полный инференс через exo-путь (шард покрывает все слои).
 */

/* Ждать RESULT(seq) от хвоста кольца, прокачивая сеть.
 * Вызывается из контекста HTTP-запроса (внутри exo_poll), поэтому
 * полный exo_poll() здесь НЕ вызываем (reentrancy exo_server_poll) —
 * только транспорт и discovery. */
static int ring_wait_result(uint32_t seq, uint32_t timeout_ms)
{
    uint64_t start = hal_timer_get_milliseconds();

    for (;;) {
        if (g_ring_req.result_ready) {
            if (g_ring_req.result_seq == seq)
                return EXO_OK;
            /* Запоздалый RESULT от предыдущего шага — сбросить и ждать */
            console_printf("[EXO] ring: stale RESULT seq=%u (want %u)\n",
                           g_ring_req.result_seq, seq);
            g_ring_req.result_ready = false;
        }
        if (hal_timer_get_milliseconds() - start > timeout_ms)
            return EXO_ERR_TIMEOUT;
        tcpip_poll();
        exo_discovery_poll();
        exo_transport_poll();
    }
}

/* Полный локальный прогон через layer-range API (шард = все слои).
 * Эквивалент streaming_inference_generate() по семантике (greedy/temp
 * настраивается теми же глобалами движка). */
static int exo_local_generate(const int *prompt_tokens, int prompt_len,
                              int *out_tokens, int max_out)
{
    int dim = 0, n_layers = 0, ctx_len = 0;
    streaming_inference_get_info(&dim, &n_layers, NULL, &ctx_len);
    if (dim <= 0 || dim > EXO_MAX_HIDDEN || n_layers <= 0)
        return -1;

    static float hidden[EXO_MAX_HIDDEN];

    int pos = 0;
    int token = prompt_tokens[0];
    int generated = 0;

    while (pos < ctx_len && generated < max_out) {
        if (streaming_inference_embed(token, hidden) != 0)
            return -1;
        if (streaming_inference_forward_layers(hidden, pos, 0, n_layers) != 0)
            return -1;

        int next_token;
        if (pos < prompt_len - 1) {
            next_token = prompt_tokens[pos + 1];
        } else {
            next_token = streaming_inference_sample_token(hidden);
            if (next_token < 0) return -1;
            out_tokens[generated++] = next_token;
            if (streaming_inference_is_stop_token(next_token)) break;
        }

        token = next_token;
        pos++;
    }
    return generated;
}

/* Кольцевой прогон: оркестратор (ring[0]) embed'ит, гоняет свой шард и
 * шлёт TENSOR по кольцу; хвост sample'ит и возвращает RESULT (барьер).
 * Требование: локальная нода — ring[0] (проверяется вызывающим). */
static int exo_ring_generate(const exo_shard_t *shard,
                             const int *prompt_tokens, int prompt_len,
                             int *out_tokens, int max_out)
{
    int dim = 0, ctx_len = 0;
    streaming_inference_get_info(&dim, NULL, NULL, &ctx_len);
    if (dim <= 0 || dim > EXO_MAX_HIDDEN)
        return -1;

    int next_idx = exo_ring_next(self_table_index());
    const exo_node_t *next = next_idx >= 0 ? exo_node_get(next_idx) : NULL;
    if (!next || next->is_local) {
        console_printf("[EXO] ring: next node unavailable\n");
        return -1;
    }

    static float hidden[EXO_MAX_HIDDEN];

    g_ring_req.active = true;
    g_ring_req.request_id = exo__next_request_id();
    g_ring_req.result_ready = false;

    console_printf("[EXO] ring generate: req=%llu prompt=%d tokens, "
                   "local layers %u..%u, next '%s'\n",
                   (unsigned long long)g_ring_req.request_id, prompt_len,
                   shard->start_layer, shard->end_layer, next->node_id);

    int pos = 0;
    int token = prompt_tokens[0];
    int generated = 0;

    while (pos < ctx_len && generated < max_out) {
        if (streaming_inference_embed(token, hidden) != 0)
            break;
        if (streaming_inference_forward_layers(hidden, pos,
                                               (int)shard->start_layer,
                                               (int)shard->end_layer) != 0)
            break;

        /* Скрытый вектор следующей ноде кольца */
        exo_tensor_msg_t hdr = {
            .magic = EXO_TENSOR_MAGIC,
            .version = EXO_PROTO_VERSION,
            .msg_type = EXO_MSG_TENSOR,
            .seq = (uint32_t)pos,
            .request_id = g_ring_req.request_id,
            .dtype = EXO_DTYPE_F32,
            .n_dims = 1,
            .dims = { (uint32_t)dim, 0, 0, 0 },
            .payload_len = (uint32_t)(dim * sizeof(float)),
        };
        if (exo_send_tensor(next->ip, next->ctrl_port, &hdr, hidden) != EXO_OK) {
            console_printf("[EXO] ring: TENSOR send failed at pos=%d\n", pos);
            break;
        }

        /* Lockstep: ждать RESULT-барьер от хвоста кольца */
        g_ring_req.result_ready = false;
        if (ring_wait_result((uint32_t)pos, EXO_RING_RESULT_TIMEOUT_MS) != EXO_OK) {
            console_printf("[EXO] ring: RESULT timeout at pos=%d\n", pos);
            break;
        }

        int next_token;
        if (pos < prompt_len - 1) {
            /* Prefill: сэмплированный хвостом токен игнорируем */
            next_token = prompt_tokens[pos + 1];
        } else {
            next_token = g_ring_req.result_token;
            if (next_token < 0) break;
            out_tokens[generated++] = next_token;
            if (g_ring_req.result_finished ||
                streaming_inference_is_stop_token(next_token))
                break;
        }

        token = next_token;
        pos++;
    }

    g_ring_req.active = false;
    console_printf("[EXO] ring generate done: %d tokens (%d pos)\n",
                   generated, pos);
    return generated;
}

int exo__chat(const char *prompt, exo_emit_fn emit, void *ctx,
              int max_tokens)
{
    if (!prompt || !emit) return EXO_ERR_PROTOCOL;

    if (!streaming_inference_is_ready()) {
        emit("[exo] inference engine not ready "
             "(load a GGUF model first, e.g. 'chat' command)", true, ctx);
        return EXO_ERR_UNSUPPORTED;
    }

    if (max_tokens <= 0) max_tokens = EXO_DEFAULT_MAX_TOKENS;
    if (max_tokens > 256) max_tokens = 256;

    /* Chat template (ChatML/[INST]/GLM autodetect) — как в команде `chat`;
     * instruct-модели без шаблона не выдают EOS и генерируют до лимита. */
    char wrapped[768];
    const char *eff_prompt = prompt;
    if (chat_template_wrap(prompt, wrapped, sizeof(wrapped)) > 0) {
        eff_prompt = wrapped;
        int stop_tok = chat_template_stop_token();
        if (stop_tok >= 0)
            streaming_inference_set_eos(stop_tok);
        /* TODO(ring): хвост кольца использует СВОЙ eos из GGUF-метаданных;
         * для моделей, где stop-токен шаблона != eos модели, протащить
         * stop-набор в PROMPT/RESULT-протокол. */
    }

    /* Токенизация промпта */
    int prompt_tokens[512];
    int n_prompt = bpe_tokenizer_encode(eff_prompt, prompt_tokens, 512,
                                        false, false);
    if (n_prompt <= 0) {
        emit("[exo] tokenization failed", true, ctx);
        return EXO_ERR_PROTOCOL;
    }

    /* Выбор пути: кольцо, если шард назначен и нод больше одной.
     * Оркестратор обязан быть ring[0] (embedding + запуск прохода);
     * иначе — корректный fallback на полный локальный прогон (модель
     * замаплена целиком). */
    const exo_shard_t *shard = exo_shard_local();
    bool use_ring = shard && shard->ring_size > 1;
    if (use_ring && shard->ring_index != 0) {
        console_printf("[EXO] chat on non-head node (ring %u/%u): "
                       "local fallback\n",
                       shard->ring_index, shard->ring_size);
        use_ring = false;
    }

    /* TODO(stream): посимвольная выдача — сейчас токены накапливаются
     * и эмитятся в конце одним проходом (для SSE это допустимо). */
    int out_tokens[256];
    int n_out = use_ring
        ? exo_ring_generate(shard, prompt_tokens, n_prompt, out_tokens, max_tokens)
        : exo_local_generate(prompt_tokens, n_prompt, out_tokens, max_tokens);
    if (n_out <= 0) {
        emit("[exo] generation failed", true, ctx);
        return EXO_ERR_UNSUPPORTED;
    }

    /* Финальный stop-токен (<|im_end|> и т.п.) в выдачу не входит */
    while (n_out > 0 && streaming_inference_is_stop_token(out_tokens[n_out - 1]))
        n_out--;

    /* Декодировать BPE целиком (Ġ→пробелы и т.п.), как команда `chat` */
    static char text[2048];
    int len = bpe_tokenizer_decode(out_tokens, n_out, text, sizeof(text));
    if (len > 0) {
        emit(text, false, ctx);
    } else {
        for (int i = 0; i < n_out; i++) {
            const char *piece = streaming_inference_get_token(out_tokens[i]);
            emit(piece ? piece : "", false, ctx);
        }
    }
    emit("", true, ctx);
    return EXO_OK;
}
