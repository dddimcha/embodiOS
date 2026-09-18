# kernel/exo — распределённый инференс (exo-style) для embodiOS

**Статус: LIVE INFERENCE** (ветка feat/exo-live). Кольцевой проход токена
(PROMPT→TENSOR→RESULT) работает на реальном инференсе: `exo_forward_shard`
гоняет свой диапазон слоёв через layer-range API streaming_inference.c,
хвост кольца sample'ит токен. Однонодный OpenAI API проверен curl'ом
(ответ «The capital of France is Paris.»).

Модуль распределённого инференса по кольцу нод (pipeline parallelism
по слоям), по мотивам классической архитектуры [exo](https://github.com/exo-explore/ex-exo).
Подробный гид и обоснование дизайна: `docs/PORTING_GLM_EXO.md`.

## Что работает (проверено в QEMU, virtio-net + hostfwd)

- Сборка и загрузка ядра с модулем (с embedded SmolLM и без).
- `exo [id] [port]` — init ноды: node_id из MAC, local IP из tcpip,
  discovery (udp/5678) + tensor transport (tcp/50051).
- `exonodes` — таблица пиров (локальная нода).
- `exoshard [model n_layers]` — ring memory weighted partitioning,
  назначение/просмотр шарда.
- `exopeer <id> <ip> <port> <ram_mb>` — статический пир для транспортов
  без broadcast (QEMU user-net изолирует гостей; пир через hostfwd:
  `10.0.2.2:<hostport>` второй ноды). Pinned-пиры не истекают по таймауту.
- `setip <ip> [netmask] [gateway]` — статический IPv4 (point-to-point
  линки `-netdev socket`, где обе ноды по умолчанию 10.0.2.15).
- `exoserve [port|stop]` — OpenAI API: `GET /healthcheck`, `GET /v1/models`,
  `POST /v1/chat/completions` (JSON и SSE; `max_tokens` из запроса,
  дефолт 64). Ответ — реальный инференс: chat template, генерация до
  EOS, BPE-декодирование (как команда `chat`).
- **Live shard forward**: `exo_forward_shard()` →
  `streaming_inference_forward_layers(hidden, pos, start, end)`; KV-cache
  слоёв шарда локален (индексация по абсолютному номеру слоя), по кольцу
  ходит только hidden-вектор (n_embd float32 на токен на hop).
- **Кольцевая генерация** (lockstep): оркестратор (обязан быть ring[0])
  для каждого токена: embed → свои слои → TENSOR следующей ноде → ждёт
  RESULT-барьер от хвоста (final norm + lm_head + sampling на хвосте).
  Одна нода = полный инференс через тот же exo-путь (шард [0, n_layers)).
  Оркестратор не-ring[0] → корректный fallback на полный локальный прогон.
- Discovery announce: UDP broadcast 255.255.255.255:5678 каждые 2.5 с
  реально уходит на провод (tx-счётчики растут, виден в pcap).
- `exo_poll()` вызывается из главного цикла ядра в простое
  (kernel_loop переведён на неблокирующий посимвольный ввод).

## Сопутствующие исправления инфраструктуры (без них exo не работал)

- `net/tcpip.c`: broadcast TX (255.255.255.255 и subnet broadcast → eth
  FF:FF:FF:FF:FF:FF без ARP, `resolve_dst_mac()`), приём subnet broadcast
  в `handle_ip()`, геттеры `tcpip_get_local_ip()/tcpip_get_local_mac()`,
  `socket_recvfrom()` (очередь метаданных UDP-датаграмм).
- `net/tcpip.c` TCP: настоящий TCP checksum (RFC 1071 pseudo-header;
  раньше был 0 — slirp/host дропал наши сегменты), корректный server
  handshake (re-SYN+ACK на дубль SYN, финальный ACK → ESTABLISHED,
  SYN consumes seq), ACK на данные + in-order guard, двухпроходный поиск
  сокета (SYN не попадает в умирающее соединение), `socket_accept` из
  ESTABLISHED.
- `drivers/net/virtio_net.c`: TX-completion — drain used-ring по elem->id
  и time-based ожидание (100 мс); прежний busy-wait 100k итераций
  освобождал дескриптор, ещё видимый устройству в avail ring, — QEMU
  падал с "zero sized buffers are not allowed" и ломал RX-доставку.
- `core/kernel.c`: `hal_timer_init()` в `kernel_main` (без него
  `hal_timer_get_milliseconds()` всегда 0 — не тикали интервалы discovery
  и все таймауты tcpip/exo).


## Файлы

| Файл | Назначение |
|---|---|
| `../include/embodios/exo.h` | Публичный API: `exo_init`, discovery, шардирование, транспорт, HTTP-сервер |
| `exo_internal.h` | Внутренние связи между файлами модуля (не API) |
| `exo_node.c` | Состояние ноды, `exo_init`/`exo_poll`, оркестратор кольца, `exo_forward_shard` (пока passthrough), локальная генерация через streaming_inference |
| `exo_discovery.c` | UDP broadcast discovery: анонсы JSON на порт 5678 каждые 2.5 с, таблица пиров, таймаут 30 с |
| `exo_shard.c` | Ring memory weighted partitioning: сортировка нод по RAM, доли слоёв, rebalance при join/leave |
| `exo_transport.c` | Бинарный TCP-протокол тензоров (48-байтный заголовок + payload, network byte order), замена gRPC NodeService |
| `exo_server.c` | Минимальный OpenAI-совместимый HTTP API: `POST /v1/chat/completions` (JSON и SSE), `GET /v1/models`, `/healthcheck` |

## Сборка

Файлы модуля добавлены в `KERNEL_C_SOURCES` (kernel/Makefile, секция net):

```make
    exo/exo_node.c \
    exo/exo_discovery.c \
    exo/exo_shard.c \
    exo/exo_transport.c \
    exo/exo_server.c \
```

## Shell-команды (core/stubs.c)

| Команда | Действие |
|---|---|
| `exo [id] [port]` | `exo_init(id, port)` + печать статуса ноды |
| `exonodes` | дамп таблицы пиров (`exo_node_count`/`exo_node_get`) |
| `exoshard [model] [n_layers]` | просмотр шарда / `exo_shard_assign(model, n, EXO_STRATEGY_RING_MEMORY_WEIGHTED)` |
| `exoserve [port|stop]` | старт/стоп OpenAI API (`exo_serve_openai`/`exo_serve_stop`) |

`exo_poll()` вызывается из главного цикла (`kernel_loop`, core/kernel.c)
в ветке простоя — discovery и серверы обслуживаются, пока shell ждёт ввода.

## Известные TODO до полной функциональности

1. ~~Диапазон слоёв в streaming_inference.c~~ — ✅ layer-range API:
   `streaming_inference_embed/forward_layers/sample_token/is_stop_token`
   (см. streaming_inference.h); `exo_forward_shard` больше не passthrough.
2. **Шард-загрузчик GGUF**: мапить только `blk.[start..end)` вместо всех слоёв
   (MAP_LAYER_TENSOR, ai/streaming_inference.c); embedding только на
   ring[0], output/output_norm — на хвосте кольца. Сейчас каждая нода
   держит модель целиком — корректно, но не экономит RAM.
3. ~~Sampling на хвосте кольца~~ — ✅ `streaming_inference_sample_token()`
   (final norm + lm_head + temperature/top-p) вызывается хвостом на
   каждый TENSOR; RESULT (token + finished) уходит оркестратору.
4. **Ограничения сокетов**: `socket_accept()` возвращает тот же fd (listener
   пересоздаётся после каждого соединения); rx_buffer 4096 и отсутствие
   переупорядочивания TCP-сегментов ограничивают throughput — для серьёзной
   нагрузки нужен апгрейд tcpip.c (MSS-сегментация, окна, reassembly,
   ретрансмиты). Из-за одно-соединение-на-listener кольцо работает в
   lockstep (RESULT = барьер на каждый токен), pipelining prefill — TODO.
5. **Двухнодный прогон — ЧАСТИЧНО (блокер задокументирован)**.
   Топология: два QEMU (user-net), статические пиры через hostfwd-меш
   (A → 10.0.2.2:25052 → host → B:50051 и обратно). Работает: статические
   пиры в таблицах обеих нод (`exonodes` — 2 ноды), согласованный разрез
   `exoshard smollm 30 even` → A: слои 0..15 (ring 0/2), B: 15..30 (1/2)
   на ОБЕИХ нодах; оркестратор A запускает кольцо (`ring generate: req=1
   prompt=16 tokens, local layers 0..15, next 'nodeB'`) и отправляет TENSOR
   (connect+write через двойной slirp — OK). Блокер: гость B не принимает
   входящие TCP-соединения (SYN через hostfwd достигает хост-листенера, но
   гость B не логирует accept; гость A с идентичным бинарником принимает
   нормально — проверено curl'ом на A:50051 → 'tensor connection from').
   Подозрение: RX-доставка virtio-net на втором инстансе QEMU (B: -m 1536M,
   TSC not invariant) — результат: RESULT timeout на A, ответ
   «[exo] generation failed». TODO: диагностика virtio RX на B (tcpip_poll
   счётчики пакетов), затем повторить кольцо; также поправить таймер
   (RESULT timeout 180s сработал за ~89s wall-clock под TCG-контенцией).
   Настоящий broadcast discovery между гостями: slirp изолирует broadcast —
   для него нужен `-netdev socket` p2p-линк (нодам нужны разные IP — `setip`).
6. **Оркестратор вне ring[0]**: relay PROMPT к ring[0] и маршрутизация
   RESULT на HTTP-ноду не реализованы (fallback: локальный прогон).
   EOS/stop-набор хвоста кольца — из его GGUF-метаданных; если stop-токен
   шаблона ≠ eos модели, протащить stop-набор в протокол (сейчас для
   SmolLM ChatML совпадает: <|im_end|>).
7. **Dtype f16 на проводе**: сейчас hidden-вектор передаётся как f32
   (2.3 КБ/токен/hop для SmolLM-135M; для 32B — 24 КБ, ~17 чанков по 1400).
