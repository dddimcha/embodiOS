/* EMBODIOS exo-style Distributed Inference API
 *
 * Распределённый инференс LLM по кольцу нод (pipeline parallelism по слоям),
 * по мотивам классической архитектуры exo (exo-explore/ex-exo):
 *
 *   - UDP broadcast discovery (порт 5678, JSON-анонсы, таймаут 30 с)
 *   - Ring memory weighted partitioning (Shard{start,end,n_layers})
 *   - Бинарный TCP-протокол тензоров (замена gRPC NodeService)
 *   - OpenAI-совместимый HTTP API (/v1/chat/completions, SSE)
 *
 * Все функции вызываются из контекста ядра (polling mode); сетевой стек —
 * net/tcpip.c (tcpip_poll() должен вызываться периодически, см. exo_poll()).
 *
 * Файлы реализации: kernel/exo/exo_node.c, exo_discovery.c, exo_shard.c,
 * exo_transport.c, exo_server.c.
 */

#ifndef EMBODIOS_EXO_H
#define EMBODIOS_EXO_H

#include <embodios/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Константы протокола
 * ============================================================================ */

#define EXO_DISCOVERY_PORT      5678    /* UDP broadcast discovery (как в exo) */
#define EXO_DEFAULT_CTRL_PORT   50051   /* TCP: тензорный трафик (замена gRPC) */
#define EXO_DEFAULT_API_PORT    52415   /* HTTP: OpenAI-совместимый API (как в exo) */

#define EXO_ANNOUNCE_INTERVAL_MS    2000    /* период анонсов (~2 с, live discovery) */
/* Таймаут пира. Под TCG один прогон шарда (блокирующий, мейн-луп не
 * обслуживается) занимает десятки секунд — beacon'ы в это время не шлются
 * из цикла, поэтому: (a) beacon отправляется и на каждый входящий TENSOR
 * (exo_discovery_heartbeat из handle_tensor), (b) любой тензорный трафик
 * от пира обновляет last_seen (exo_discovery_touch_ip из transport poll),
 * (c) таймаут увеличен до 120 с — больше worst-case латентности позиции
 * кольца под TCG; mid-generation сбой ловится RESULT-таймаутом (180 с),
 * а не expire. Классическое значение exo — 30 с (быстрое железо). */
#define EXO_NODE_TIMEOUT_MS         120000  /* таймаут пира (TCG-scaled) */

#define EXO_MAX_NODES           16      /* макс. нод в кольце */
#define EXO_NODE_ID_LEN         32      /* длина строки node_id */
#define EXO_MODEL_ID_LEN        64      /* длина идентификатора модели */

/* Magic тензорного протокола: "EXOT" */
#define EXO_TENSOR_MAGIC        0x45584F54u
#define EXO_PROTO_VERSION       1

/* Типы сообщений бинарного протокола (аналог rpc NodeService) */
typedef enum {
    EXO_MSG_PROMPT      = 1,    /* SendPrompt: payload = токены (int32) */
    EXO_MSG_TENSOR      = 2,    /* SendTensor: payload = активации */
    EXO_MSG_RESULT      = 3,    /* SendResult: payload = токен(ы) + is_finished */
    EXO_MSG_HEALTH      = 4,    /* HealthCheck (ping/pong) */
    EXO_MSG_TOPOLOGY    = 5,    /* CollectTopology (gossip) */
} exo_msg_type_t;

/* Типы данных активаций, передаваемых по кольцу */
typedef enum {
    EXO_DTYPE_F32   = 0,
    EXO_DTYPE_F16   = 1,
    EXO_DTYPE_BF16  = 2,
    EXO_DTYPE_I32   = 3,        /* токены в EXO_MSG_PROMPT/RESULT */
} exo_dtype_t;

#define EXO_TENSOR_MAX_DIMS 4

/* ============================================================================
 * Структуры
 * ============================================================================ */

/* Заголовок тензорного сообщения (on-wire, network byte order).
 * Полный размер — 48 байт, за ним следует payload_len байт payload. */
typedef struct exo_tensor_msg {
    uint32_t magic;                 /* EXO_TENSOR_MAGIC */
    uint16_t version;               /* EXO_PROTO_VERSION */
    uint16_t msg_type;              /* exo_msg_type_t */
    uint32_t seq;                   /* номер сообщения в рамках request_id */
    uint64_t request_id;            /* сквозной id запроса */
    uint32_t dtype;                 /* exo_dtype_t */
    uint32_t n_dims;                /* число измерений (<= EXO_TENSOR_MAX_DIMS) */
    uint32_t dims[EXO_TENSOR_MAX_DIMS];
    uint32_t payload_len;           /* байт payload после заголовка */
} __packed exo_tensor_msg_t;

/* Описание ноды кластера (аналог DeviceCapabilities + адрес в exo) */
typedef struct exo_node {
    char     node_id[EXO_NODE_ID_LEN];  /* уникальный id (строка) */
    uint32_t ip;                        /* IPv4 (host byte order) */
    uint16_t ctrl_port;                 /* TCP-порт тензорного сервиса */
    uint64_t ram_total;                 /* байт, всего */
    uint64_t ram_free;                  /* байт, свободно (для weighted partition) */
    char     model_id[EXO_MODEL_ID_LEN];/* какая модель развёрнута (caps) */
    uint64_t flops_fp16;                /* оценка производительности (опц.) */
    uint64_t last_seen_ms;              /* последний heartbeat */
    bool     is_local;                  /* эта нода — мы сами */
    bool     active;                    /* слот занят */
    bool     pinned;                    /* статический пир (exopeer):
                                         * не истекает по таймауту */
} exo_node_t;

/* Шард модели: диапазон слоёв [start_layer, end_layer) (как Shard в exo) */
typedef struct exo_shard {
    char     model_id[EXO_MODEL_ID_LEN];
    uint32_t start_layer;           /* первый локальный слой (inclusive) */
    uint32_t end_layer;             /* последний локальный слой (exclusive) */
    uint32_t n_layers;              /* всего слоёв в модели */
    uint32_t ring_index;            /* позиция ноды в кольце (0 = embedding) */
    uint32_t ring_size;             /* число нод в кольце */
} exo_shard_t;

/* Стратегии партиционирования */
typedef enum {
    EXO_STRATEGY_RING_MEMORY_WEIGHTED = 0,  /* доля слоёв ~ ram ноды (дефолт exo) */
    EXO_STRATEGY_RING_EVEN          = 1,    /* поровну (для отладки) */
} exo_shard_strategy_t;

/* ============================================================================
 * Жизненный цикл узла (exo_node.c)
 * ============================================================================ */

/**
 * Инициализация exo-ноды.
 * @param node_id     уникальный id ноды (NULL → сгенерировать из MAC/IP)
 * @param listen_port TCP-порт тензорного сервиса (0 → EXO_DEFAULT_CTRL_PORT)
 * @return 0 при успехе, <0 при ошибке
 * Требует: tcpip_init()/tcpip_configure() уже выполнены (ядро делает это
 * в kernel_main при наличии virtio_net/e1000e).
 */
int exo_init(const char *node_id, uint16_t listen_port);

/** Завершение работы ноды (закрыть сокеты, очистить таблицы). */
void exo_shutdown(void);

/**
 * Главный polling-шаг: tcpip_poll + discovery + тензорный сервер + HTTP API.
 * Вызывать из kernel loop наряду с schedule().
 */
void exo_poll(void);

/** true, если exo_init() успешно выполнен */
bool exo_is_running(void);

/* ============================================================================
 * Discovery (exo_discovery.c) — UDP broadcast, порт 5678
 * ============================================================================ */

/**
 * Запустить discovery: открыть UDP-сокет на EXO_DISCOVERY_PORT.
 * Вызывается из exo_init().
 */
int exo_discovery_start(void);

/**
 * Отправить JSON-анонс этой ноды broadcast'ом (255.255.255.255:5678).
 * Формат (как в exo): {"type":"discovery","node_id":...,"ctrl_port":...,
 *                      "memory":...,"memory_free":...,"model":...}
 * @return 0 при успехе, <0 при ошибке
 */
int exo_discovery_announce(void);

/**
 * Принять и разобрать входящие анонсы; обновить таблицу пиров;
 * вычистить пиров по таймауту EXO_NODE_TIMEOUT_MS; при необходимости
 * отправить свой анонс (по EXO_ANNOUNCE_INTERVAL_MS).
 * Вызывается из exo_poll().
 */
void exo_discovery_poll(void);

/** Число активных нод в таблице (включая локальную). */
int exo_node_count(void);

/** Получить ноду по индексу [0, exo_node_count()) или NULL. */
const exo_node_t* exo_node_get(int index);

/** Найти ноду по node_id или NULL. */
const exo_node_t* exo_node_find(const char *node_id);

/**
 * Добавить статического пира вручную (команда exopeer).
 * Для транспортов, где broadcast discovery не работает (QEMU user-net/slirp
 * изолирует гостей друг от друга): пир адресуется явно, напр. через hostfwd
 * (ip = 10.0.2.2, port = host-порт, проброшенный на ctrl_port второй ноды).
 * Статический пир не истекает по EXO_NODE_TIMEOUT_MS.
 * @param ram_mb  RAM пира в МБ (участвует в weighted partitioning)
 * @return 0 при успехе, <0 при ошибке
 */
int exo_discovery_add_peer(const char *node_id, uint32_t ip,
                           uint16_t ctrl_port, uint64_t ram_mb);

/**
 * Распечатать живую таблицу discovery на консоль (команда exodiscover):
 * node_id, ip:ctrl_port, ram_free (MB), возраст последнего heartbeat (с),
 * флаги (local/pinned).
 */
void exo_discovery_print_table(void);

/**
 * Heartbeat из горячего пути (handle_tensor): отправить анонс, если
 * интервал EXO_ANNOUNCE_INTERVAL_MS истёк. Нужен, т.к. во время
 * блокирующего прогона слоёв мейн-луп (и exo_poll) не выполняется.
 */
void exo_discovery_heartbeat(void);

/**
 * Обновить last_seen пира по IP (любой тензорный трафик = признак
 * живости). Вызывается из exo_transport_poll при приёме сообщений.
 */
void exo_discovery_touch_ip(uint32_t ip);

/* ============================================================================
 * Шардирование (exo_shard.c) — ring memory weighted partitioning
 * ============================================================================ */

/**
 * Назначить шард локальной ноде по текущей таблице пиров.
 * Ноды сортируются по ram_free (убывание) → порядок в кольце;
 * доля слоёв ноды = ram_free_i / sum(ram_free); слои идут подряд,
 * кольцо замыкается. Локальная нода обязана присутствовать в таблице.
 * @param model_id  идентификатор модели (должен совпадать у всех нод)
 * @param n_layers  число слоёв модели (из GGUF block_count)
 * @param strategy  стратегия
 * @return 0 при успехе (локальный шард доступен через exo_shard_local())
 */
int exo_shard_assign(const char *model_id, uint32_t n_layers,
                     exo_shard_strategy_t strategy);

/** Локальный шард (валиден после exo_shard_assign()). */
const exo_shard_t* exo_shard_local(void);

/**
 * Пересчитать шарды (при join/leave ноды). Эквивалент exo_shard_assign
 * с сохранёнными параметрами; сбрасывает незавершённые запросы.
 */
int exo_shard_rebalance(void);

/**
 * Следующая нода в кольце после node_idx (по модулю ring_size).
 * @return индекс в таблице нод или -1
 */
int exo_ring_next(int node_idx);

/**
 * Построить кольцо из живой таблицы discovery (команда `exoring auto`):
 * сортировка по ram_free (убывание), при равенстве — детерминированный
 * tiebreak по node_id (strncmp), поэтому ВСЕ ноды вычисляют одно и то же
 * кольцо. Печатает порядок кольца; ring[0] — оркестратор (max ram_free).
 * Шарды не назначаются (это делает exoshard); если шард уже был назначен,
 * кольцо пересобирается с сохранёнными параметрами модели.
 * @return 0 при успехе, <0 при ошибке (пустая таблица)
 */
int exo_ring_auto(void);

/**
 * Автоназначение шардов (команда `exoshard auto`): RAM-weighted split
 * (EXO_STRATEGY_RING_MEMORY_WEIGHTED) по кольцу из exo_ring_auto.
 * model_id/n_layers берутся из загруженной GGUF (или из предыдущего
 * назначения); если модель не загружена — ошибка (нужен warm-up `chat`).
 * @return 0 при успехе, <0 при ошибке
 */
int exo_shard_auto(void);

/**
 * Пометить кольцо деградировавшим (таймаут/обрыв TENSOR/RESULT
 * посреди генерации). Флаг сбрасывается при успешном exo_shard_assign()
 * (новое кольцо после rebalance/re-join пира).
 */
void exo_ring_mark_degraded(const char *reason);

/** true, если кольцо помечено деградировавшим. */
bool exo_ring_is_degraded(void);

/* ============================================================================
 * Прогон локальных слоёв (exo_node.c)
 * ============================================================================ */

/**
 * Прогнать локальный диапазон слоёв [shard.start_layer, shard.end_layer)
 * над скрытым состоянием одного токена.
 *
 * @param hidden_in   входной вектор (n_embd float32)
 * @param len         длина вектора (= n_embd модели)
 * @param hidden_out  выходной вектор (n_embd float32), может совпадать с in
 * @param pos         позиция токена (для RoPE и KV-cache)
 * @return 0 при успехе, <0 при ошибке
 *
 * Реальный прогон через streaming_inference_forward_layers() (layer-range
 * API, см. streaming_inference.h): KV-cache слоёв шарда локален для ноды,
 * по кольцу идёт только скрытый вектор. Требует загруженной модели
 * (streaming_inference_is_ready()) и назначенного шарда.
 */
int exo_forward_shard(const float *hidden_in, size_t len,
                      float *hidden_out, uint32_t pos);

/* ============================================================================
 * Тензорный транспорт (exo_transport.c) — TCP поверх net/tcpip.c
 * ============================================================================ */

/**
 * Запустить TCP-сервер тензорного сервиса (listen на ctrl_port).
 * Вызывается из exo_init().
 */
int exo_transport_listen(uint16_t port);

/**
 * Отправить тензорное сообщение (заголовок + payload) ноде по TCP.
 * Length-prefixed протокол: 48-байтный exo_tensor_msg_t (network byte order),
 * затем payload_len байт. Данные режутся на сегменты <= MTU из-за
 * ограничения tx_buffer в tcpip.c (ETH_FRAME_MAX).
 * @param dst_ip    IP получателя (host byte order)
 * @param dst_port  ctrl_port получателя
 * @param hdr       заголовок (host byte order; конверсия внутри)
 * @param payload   payload (может быть NULL при payload_len == 0)
 * @return 0 при успехе, <0 при ошибке/таймауте
 */
int exo_send_tensor(uint32_t dst_ip, uint16_t dst_port,
                    const exo_tensor_msg_t *hdr, const void *payload);

/**
 * Принять тензорное сообщение из открытого соединения.
 * @param fd            сокет (socket_accept/socket_connect)
 * @param hdr           out: заголовок (host byte order)
 * @param payload       out: буфер под payload (NULL — только заголовок)
 * @param payload_cap   ёмкость буфера payload
 * @param timeout_ms    таймаут ожидания (0 = без таймаута)
 * @return 0 при успехе, <0 при ошибке/таймауте/несоответствии magic
 */
int exo_recv_tensor(int fd, exo_tensor_msg_t *hdr,
                    void *payload, size_t payload_cap, uint32_t timeout_ms);

/**
 * Polling-шаг тензорного сервера: accept новых соединений, диспетчеризация
 * входящих сообщений оркестратору. Вызывается из exo_poll().
 */
void exo_transport_poll(void);

/* ============================================================================
 * OpenAI-совместимый HTTP API (exo_server.c)
 * ============================================================================ */

/**
 * Запустить HTTP-сервер (порт по умолчанию EXO_DEFAULT_API_PORT).
 * Эндпоинты (stub-уровень):
 *   POST /v1/chat/completions  — принять JSON, извлечь prompt, сгенерировать
 *                                (локально или по кольцу), отдать SSE-чанки
 *   GET  /v1/models            — список моделей (одна — текущая)
 *   GET  /healthcheck          — {"status":"ok"}
 * @return 0 при успехе, <0 при ошибке
 */
int exo_serve_openai(uint16_t port);

/** Остановить HTTP-сервер (закрыть listen-сокет). */
void exo_serve_stop(void);

/** true, если HTTP-сервер слушает порт. */
bool exo_server_running(void);

/** Порт, на котором слушает HTTP-сервер (0 — не запущен). */
uint16_t exo_server_port(void);

/** Polling-шаг HTTP-сервера. Вызывается из exo_poll(). */
void exo_server_poll(void);

/* ============================================================================
 * Коды ошибок
 * ============================================================================ */

#define EXO_OK              0
#define EXO_ERR_INIT        -1
#define EXO_ERR_NOMEM       -2
#define EXO_ERR_TIMEOUT     -3
#define EXO_ERR_NET         -4      /* ошибка tcpip-слоя */
#define EXO_ERR_PROTOCOL    -5      /* невалидное сообщение */
#define EXO_ERR_NOSHARD     -6      /* шард не назначен */
#define EXO_ERR_UNSUPPORTED -7      /* неподдерживаемая операция (stub) */

#ifdef __cplusplus
}
#endif

#endif /* EMBODIOS_EXO_H */
