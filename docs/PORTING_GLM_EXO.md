# Портирование GLM (GGUF) и exo на embodiOS

Исчерпывающий гид: текущее состояние LLM-рантайма embodiOS, пошаговый план
поддержки моделей GLM-4 (архитектуры GGUF `chatglm` и `glm4`) и план
exo-подобного распределённого инференса по кольцу нод.

Связанные материалы:
- `kernel/exo/` — компилируемый skeleton модуля распределённого инференса
  (эта ветка, `feature/exo-skeleton`);
- `kernel/exo/README.md` — включение модуля в сборку и TODO интеграции;
- исследование по GLM/exo вошло в §3-4 этого документа (источники:
  llama.cpp `src/models/chatglm.cpp`, `src/models/glm4.cpp`, exo
  `exo/orchestration/node.py`, `node_service.proto` и др.).

---

## 1. Текущее состояние рантайма embodiOS

### 1.1 Главная цепочка инференса

```
kernel_loop (core/kernel.c:371)
  → process_command (core/stubs.c:108)          # команды chat/talk/stream
  → gguf_load_model (ai/gguf_loader.c:294)
  → gguf_parser_load (ai/gguf_parser.c:1434)    # GGUF v3, метаданные+тензоры
  → bpe_tokenizer_init (ai/bpe_tokenizer.c)     # BPE из tokenizer.ggml.tokens
  → streaming_inference_init/generate (ai/streaming_inference.c:2037/2291)
```

### 1.2 Что поддерживается (ai/streaming_inference.c, ~2600 строк)

- **Формат**: GGUF v3 (v1/GGML отвергается), тензоры мапятся указателями в
  образ модели, веса остаются квантованными, dequant на лету.
- **Кванты** (`stream_dequant`): F32, F16, Q4_0, Q4_1, Q5_0, Q8_0,
  Q2_K, Q3_K, Q4_K, Q5_K, Q6_K, IQ4_NL (Q3_K/Q5_K/IQ4_NL добавлены в
  feat/kquants, ggml-точные порты). Нет: Q5_1, Q8_1 (используется только
  как формат активаций), остальное IQ-семейство.
- **Матмул**: `matmul_stream` (:1246) — fused-пути Q8_0×Q8_1 и Q4_K×Q8_1
  (SSE2/AVX2/NEON), иначе dequant+dot. Веса в GGUF-раскладке
  `[in_features, out_features]` (строка = выходной канал).
- **Архитектура**: llama-стиль — раздельные `blk.N.attn_q/k/v/output`,
  `ffn_gate/up/down`, RMSNorm pre-norm (`rmsnorm_stream`, :1155), RoPE на все
  измерения головы (`rope`, :1816, **interleaved/GPT-J-пары (i, i+1)**),
  GQA (`kv_mul`), tied embeddings, KV-cache на все слои.
- **Sampling**: greedy argmax по умолчанию; temperature и top-p задаются
  через `streaming_inference_set_temperature/top_p` (команды `temp`/`topp`).
- **Chat-template**: есть (`ai/chat_template.c`) — ChatML/[INST]/GLM
  автодетект, команда `chatformat`.
- **Attention bias**: отсутствует (`attn_q/k/v.bias` не читаются).

### 1.3 Ключевые API и структуры (точки расширения)

| Компонент | Файл:строка | Сигнатура / содержимое |
|---|---|---|
| Парсер GGUF | ai/gguf_parser.c | `int gguf_parser_load(const void *data, size_t size)` |
| Метаданные | include/embodios/gguf_parser.h | `const struct gguf_model_arch *gguf_parser_get_arch(void)` — поля `general_architecture[64]`, `block_count`, `embedding_length`, `feed_forward_length`, `attention_head_count(_kv)`, `attention_layer_norm_rms_epsilon`, `rope_dimension_count`, `rope_freq_base`, `eos_token_id`, `tokenizer_model[64]` и др. |
| Поиск тензора | gguf_parser.h | `const struct gguf_tensor_info *gguf_parser_get_tensor_by_name(const char *name)`, `const void *gguf_parser_get_tensor_data_ptr(const struct gguf_tensor_info *info)` |
| Маппинг слоёв | streaming_inference.c:2242–2263 | макрос `MAP_LAYER_TENSOR(field, suffix)` → `LayerWeights` (:270–289) |
| Слой | streaming_inference.c:1859 | `static void transformer_forward_stream(int token, int pos, int layer)` — один слой, pre-norm → attn → residual → FFN(SwiGLU) → residual |
| RoPE | streaming_inference.c:1816 | `static void rope(float *q, float *k, int pos, int dim, int head_dim, int kv_dim, float theta)` |
| RMSNorm | streaming_inference.c:1155 | `static void rmsnorm_stream(float *out, const float *x, const void *w_quant, int type, int size, float eps)` |
| Matmul | streaming_inference.c:1246 | `static void matmul_stream(float *out, const void *w_quant, int w_type, const float *x, int rows, int cols)` |
| Имя тензора | streaming_inference.c:171 | `static void build_layer_name(char *buf, size_t size, const char *prefix, int layer, const char *suffix)` |
| Токенизатор | ai/bpe_tokenizer.c | `int bpe_tokenizer_encode(const char *text, int *tokens, int max_tokens, bool add_bos, bool add_eos)`, `int bpe_tokenizer_decode(...)`, greedy longest-match (не полный BPE-merge) |
| Конфиг движка | streaming_inference.c:190 | `StreamingConfig g_cfg` (dim/hidden_dim/n_layers/n_heads/n_kv_heads/rope_theta/rms_norm_eps/eos/bos/arch_name) |

### 1.4 Таблица «модель → статус поддержки»

| Модель (GGUF arch) | Статус | Комментарий |
|---|---|---|
| TinyLlama-1.1B (`llama`) | ✓ работает | Без bias, RoPE full, vocab llama-BPE — основной тестовый путь |
| SmolLM-135M/360M (`llama`) | ✓ работает | Тестовая модель sandbox (Q4_K_M) |
| LLaMA-2/3 (`llama`) | ✓ (RoPE theta/масштаб из метаданных читаются) | LLaMA-3: нужен полный BPE-merge (сейчас longest-match — приближение) |
| Qwen2 / Qwen2.5 (`qwen2`) | ✗ → **после шага A3** | Обязателен QKV-bias (есть в Qwen2) — сейчас логиты неверны |
| ChatGLM2/3-6B (`chatglm`) | ✓ граф поддержан (A1–A7 реализованы) | + SentencePiece-токенизатор у ChatGLM3 (не реализован) |
| GLM-4-9B-Chat, GLM-Edge-1.5B/4B (`chatglm`) | ✓ supported (синтетика: логиты = numpy-эталон, F32+Q8_0) | Слитый attn_qkv+bias, слитый ffn_up (SwiGLU seq), partial rotary (interleaved); zai-org glm-edge GGUF идёт с раздельными q/k/v без bias — fallback это покрывает |
| GLM-4-9B/32B-0414 (`glm4`) | ✓ supported (синтетика: логиты = numpy-эталон, F32+Q8_0) | + post-norms; у 32B bias отсутствует — загрузчик терпит оба варианта |
| GLM-4.5/Air (`glm4moe`) | вне области | MoE — отдельный большой проект |
| Mistral/Mixtral (`mistral`) | проверка | Sliding window attention не поддержан; без SWA — близко к llama |
| Phi-2/3 (`phi`) | проверка | Phi-2: LayerNorm (не RMS) + bias — нужны оба расширения; Phi-3: llama-подобен |

---

## 2. Фаза A: порт GLM (локальный инференс GGUF)

GLM в GGUF существует в трёх архитектурах: `chatglm` (ChatGLM2/3-6B,
GLM-4-9B-Chat, GLM-Edge), `glm4` (GLM-4-0414 9B/32B — целевая для кольца),
`glm4moe` (GLM-4.5, вне области). Для 32B-в-кольце релевантен именно `glm4`.

### 2.1 Отличия от llama-стиля (сводка)

| Особенность | `chatglm` (GLM-4-9B-Chat) | `glm4` (GLM-4-0414) |
|---|---|---|
| QKV | **слитый** `blk.N.attn_qkv.weight` `[n_embd, n_embd+2·n_embd_gqa]` **+ `.bias`** | раздельные `attn_q/k/v.weight`, bias по конфигу (9B: есть, 32B: нет) |
| FFN | **слитый** `blk.N.ffn_up` `[n_embd, 2·n_ff]`, SwiGLU seq-split (первая половина — gate, вторая — up); `ffn_gate` отсутствует | то же |
| RoPE | **partial**: `rope.dimension_count = head_dim·0.5`, пары **interleaved `(i, i+1)`** (llama.cpp `LLAMA_ROPE_TYPE_NORM` для CHATGLM и GLM4-text; HF GLM4 делает то же через `repeat_interleave(2)` — НЕ NeoX, поправка к раннему ресерчу), theta = 10000·rope_ratio | то же |
| Нормы | pre-RMSNorm только | **+ `attn_post_norm` и `ffn_post_norm`** (RMSNorm внутри residual-ветки после attention и после FFN) |
| Токенизатор | GPT-2 BPE, pre-tokenizer `chatglm-bpe` | GPT-2 BPE, pre-tokenizer `glm4` (тот же regex) |
| eos/eot | `<|endoftext|>`=151329 (eos), `<|user|>`=151336 (eot), `<|assistant|>`=151338 | то же + bos=`<|endoftext|>`, add_bos=false |
| Chat-формат | `[gMASK]<sop><|system|>\n…<|user|>\n…<|assistant|>\n` | то же семейство маркеров |

### 2.2 Шаги порта

#### A1 — Метаданные (S) — ✅ сделано (ветка feat/glm-arch)

`gguf_parser.c` уже читает `<arch>.context_length`, `embedding_length`,
`block_count`, `feed_forward_length`, `attention.head_count(_kv)`,
`attention.layer_norm_rms_eps`, `rope.dimension_count`, `rope.freq_base` —
ключи строятся от строки `general.architecture`, поэтому `chatglm`/`glm4`
подхватываются автоматически теми же ключами (`glm4.context_length` и т.д.).

Что сделать:
- Убедиться, что `general.architecture` = `chatglm`/`glm4` не отвергается
  на входе (проверить ветку валидации arch в `gguf_parser_load`,
  ai/gguf_parser.c:1434+, и в `streaming_inference_init`,
  ai/streaming_inference.c:2037+ — arch_name сохраняется в
  `g_cfg.arch_name[64]`, но по нему нет ветвления).
- В `StreamingConfig` (streaming_inference.c:190) добавить:
  `int rope_n_dims` (из `arch->rope_dimension_count`, 0 = полный rotary),
  `bool has_qkv_bias`, `bool fused_qkv`, `bool fused_ffn_up`,
  `bool has_post_norms` — флаги выставлять по `g_cfg.arch_name`.

Затронуто: `gguf_parser.c` (возможно, ничего), `streaming_inference.c`
(`streaming_inference_init`).

#### A2 — Слитый attn_qkv и слитый ffn_up, arch `chatglm` (S–M) — ✅ сделано (вариант 1, виртуальный маппинг; хелпер `tensor_row_byte_offset`)

Тензорные имена `chatglm` отличаются от llama-стиля. Два подхода:

**Вариант 1 (рекомендуется) — виртуальный маппинг при загрузке.**
В `streaming_inference_init` (блок `MAP_LAYER_TENSOR`, :2242–2263) добавить
fallback-ветку: если `blk.N.attn_q.weight` не найден, искать
`blk.N.attn_qkv.weight`/`blk.N.attn_qkv.bias` и вычислять указатели Q/K/V
как смещения внутри слитого тензора:

- строка матрицы QKV в GGUF-раскладке `[in, out]` = выходной канал;
  блоки выходных строк идут подряд: Q = строки `[0, n_embd)`,
  K = `[n_embd, n_embd+n_kv)`, V = `[n_embd+n_kv, n_embd+2·n_kv)`.
- Для квантованных тензоров смещение строки r в байтах =
  `r · (in_dim / block_size) · sizeof(block)` — эта арифметика уже есть в
  `matmul_stream` (:1289, `byte_offset` по `row_offset`) и в
  `extract_embedding_*` (:1891). Вынести в хелпер
  `static size_t tensor_row_offset(int type, int in_dim, int row)`.
- Bias — плоский f32-вектор той же раскладки: `bias_q = bias[0..n_embd)` и т.д.
- `LayerWeights` (:270–289) расширить полями `attn_q_bias/attn_k_bias/attn_v_bias`.

Аналогично `ffn_up` `[n_embd, 2·n_ff]`: gate = строки `[0, n_ff)`, up =
`[n_ff, 2·n_ff)` — маппинг на существующие `lw->ffn_gate` / `lw->ffn_up`.

Плюс варианта 1: `transformer_forward_stream` (:1859) почти не меняется.

Затронуто: `streaming_inference.c` (`LayerWeights`, `MAP_LAYER_TENSOR`-блок,
хелпер смещений), ничего в `gguf_parser.c` (он отдаёт тензоры по имени как
есть).

#### A3 — QKV-bias в matmul-пути (S) — ✅ сделано ранее (commit 78ce660) + срезы слитого attn_qkv.bias

Сейчас bias нет нигде (ломает Qwen2 — дефолтную модель Makefile).

- После `matmul_stream(g_state.q, lw->attn_q, …)` (вызовы :1907±) добавить:
  `static void vec_add_bias(float *y, const float *bias, int n)` —
  тривиальный SIMD-цикл по образцу `elem_add_inplace_simd` (:1549).
- Применять к q/k/v при наличии `lw->attn_*_bias` (f32-тензоры, dequant не
  нужен — bias в GGUF всегда F32).
- Проверка: Qwen2.5-0.5B-Instruct (дефолт Makefile) — логиты должны
  совпасть с llama.cpp greedy.

Затронуто: `streaming_inference.c` (`transformer_forward_stream`, новый
хелпер). Этот же код переиспользуется для `glm4` (bias опционален) и `chatglm`.

#### A4 — Partial rotary (S) — ✅ сделано; ВАЖНО: стиль НЕ NeoX, а interleaved NORM (проверено по llama.cpp `llama_rope_type()`: CHATGLM и GLM4-text → LLAMA_ROPE_TYPE_NORM, и по HF `modeling_glm4.py`: `rotate_half` на `x[0::2]/x[1::2]` + `repeat_interleave(2)`). Частоты делятся на `n_rot`

Текущий `rope` (:1816) вращает все `head_dim` компонент парами `(i, i+1)`
(GPT-J interleaved). GLM требует:

1. Вращать только первые `n_rot = rope.dimension_count` компонент каждой
   головы (у GLM-4: head_dim=128, n_rot=64);
2. Порядок NeoX rotate-half: пары `(i, i + n_rot/2)`, частота
   `theta^(-2i/n_rot)` для `i ∈ [0, n_rot/2)`.

Изменение сигнатуры: `rope(float *q, float *k, int pos, int dim,
int head_dim, int kv_dim, float theta, int n_rot, bool neox)` — или
завести `rope_neox_partial()` рядом, оставив старую для llama/qwen.
Внимание: частоты делятся на `n_rot`, а не на `head_dim`
(`freq = powf(theta, -2.0f·i/n_rot)`), остаток головы `[n_rot, head_dim)`
остаётся без вращения.

theta: `rope.freq_base` уже читается (:2077, `10000·rope_ratio` конвертер
записывает в freq_base — проверить на glm-4-9b-chat: rope_ratio=1 → 10000).

Затронуто: `streaming_inference.c` (`rope`, вызов :1912).

#### A5 — Arch `glm4`: post-norms (S–M) — ✅ сделано

Для GLM-4-0414 в `transformer_forward_stream` (:1859) residual-ветки
меняются:

```
# attention:
attn_out = wo(attn)              # как сейчас, :1985±
attn_out = rmsnorm(attn_out, blk.N.attn_post_norm.weight)   # НОВОЕ
x = x + attn_out
# FFN:
ffn_out = ffn_down(swiglu(...))  # как сейчас
ffn_out = rmsnorm(ffn_out, blk.N.ffn_post_norm.weight)      # НОВОЕ
x = x + ffn_out
```

- `LayerWeights` += `attn_post_norm`, `ffn_post_norm` (+ типы; тензоры F32).
- Ветвление по `g_cfg.has_post_norms` (A1) — не ломать llama-путь.
- `attn_output.bias` у glm4 не используется; Q/K/V bias — по флагу A1
  (9B-0414: есть, 32B-0414: нет) — загрузчик обязан терпеть отсутствие.
- Тензоры `blk.N.nextn.*` (MTP/NextN) — игнорировать (пропускать).

Затронуто: `streaming_inference.c` (`LayerWeights`, маппинг,
`transformer_forward_stream`).

#### A6 — Токенизатор (M)

`bpe_tokenizer.c` сейчас — greedy longest-match по `tokenizer.ggml.tokens`
со score-приоритетом; для GLM-4 нужны:

1. **Pre-tokenizer** `chatglm-bpe`/`glm4` (оба = `LLAMA_VOCAB_PRE_TYPE_CHATGLM4`
   в llama.cpp): regex GPT-4o-стиля
   `(?:'[sS]|'[tT]|'[rR][eE]|'[vV][eE]|'[mM]|'[lL][lL]|'[dD])|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}{1,3}| ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+`.
   В ядре regex-движка нет — пишется ручной сканер (~150 строк):
   - сокращения `'s 't 're 've 'm 'll 'd` (любой регистр);
   - буквы (`\p{L}+`, Unicode: кириллица/CJK — классификация по диапазонам
     UTF-8 codepoint'ов) с опциональным префиксом-небуквой;
   - числа — группами по 1–3 цифры (`\p{N}{1,3}`);
   - пунктуация/символы с опциональным пробелом и хвостовыми \r\n;
   - пробелы: `\s*[\r\n]+`, `\s+(?!\S)` (пробелы в конце), `\s+`.
   Точка расширения: `bpe_tokenizer_encode` (ai/bpe_tokenizer.c, сигнатура в
   include/embodios/bpe_tokenizer.h:52) — предразбивка текста на куски до
   longest-match. Поле выбора — `tokenizer.ggml.pre` (пробросить в
   `gguf_model_arch`/выделенный геттер `gguf_parser_get_pre_tokenizer()`).
2. **BPE-merge**: для качества — настоящий merge-цикл по
   `tokenizer.ggml.merges` (в парсере merges читаются? — проверить; если нет,
   добавить в `gguf_parser.c` чтение `tokenizer.ggml.merges` как строкового
   массива). Longest-match приемлем как первое приближение.
3. **Спецтокены**: `<|endoftext|>` (151329, eos), `<|user|>` (151336, eot),
   `<|assistant|>` (151338) — в тексте промпта встречаются как литералы;
   longest-match по vocab их поймает, если строки в vocab есть (в GGUF они
   есть). eos уже берётся из метаданных (`arch->eos_token_id` → `g_cfg`).

#### A7 — Chat-форматтер и стоп-токены (S) — ✅ сделано: обёртка `[gMASK]<sop><|user|>\n{q}<|assistant|>`, `<sop>`/`<eop>` — special-токены в BPE, stop = `<|user|>` (metadata EOS `<|endoftext|>` сохраняется) + дополнительный stop `<|assistant|>` (streaming_inference_add_stop_token, до 4 стопов)

- Формат GLM-4-Chat: `[gMASK]<sop><|system|>\n{sys}<|user|>\n{q}<|assistant|>\n`
  (без пробелов; system-опционален). Реализация — ручной форматтер в
  команде `chat` (core/stubs.c:445) по флагу arch: собрать строку →
  `bpe_tokenizer_encode(..., add_bos=false)`.
- Стоп: генерация останавливается на любом из {151329, 151336, 151338}.
  Сейчас `streaming_inference_generate` (:2291) проверяет только
  `g_cfg.eos_token_id` — расширить до массива стоп-токенов
  (`g_cfg.stop_token_ids[4]`, заполнять в init для GLM).

#### A8 — Тесты (M) — ◐ синтетика ✅, реальные модели pending

1. ✅ **Синтетические tiny-модели** (tools/gen_tiny_chatglm.py): 2 слоя,
   dim 64, heads 4, kv 2, n_rot 8, vocab 1000, F32 и Q8_0, оба arch.
   numpy-эталон (tools/ref_chatglm.py) vs ядро в QEMU (команда
   `dbglogits`, tools/compare_logits.py): **top-8 логитов совпадают**,
   max |diff| ≤ 0.0018 (F32) / ≤ 0.026 (Q8_0, включает переквантование
   активаций в Q8_1) — PASS для chatglm и glm4.
2. ✅ **Регрессия**: SmolLM-135M chat в QEMU — «The capital of France is
   Paris.» (llama-путь без флагов не сломан).
3. ✅ **Метаданные реальной glm-edge-1.5b-chat** (zai-org, hf-mirror,
   Range-запрос первых 8 МБ): arch=`chatglm`, ключи `chatglm.*` читаются
   нашим парсером, тензоры — раздельные attn_q/k/v (без bias) + слитый
   ffn_up [2048, 12288], rope.dimension_count=128 (=head_dim → полный
   rotary, partial-путь не нужен для этой модели), vocab 59264,
   eos=59246, pre=`chatglm-bpe`. Наш fallback-маппинг покрывает оба
   варианта раскладки QKV (слитый THUDM и раздельный zai-org).
4. ⏳ **Полный прогон glm-edge-1.5b** в QEMU — заблокирован лимитом RAM
   ядра (1 GB identity-map, kernel.c:232): Q4_K_M ~0.9–1.1 ГБ + KV-cache
   ~235 МБ не влезают; Q2_K (~700 МБ) — впритык. TCG-скорость ~секунды
   на токен для 1.5B — чат возможен, но медленно.
5. ⏳ **glm-4-9b-chat Q4_K_M** (~6 GB) — требует поднятия лимита RAM.

Суммарно фаза A: **M** (много S-шагов поверх существующего рантайма).

---

## 3. Фаза B: exo-подобное кольцо нод

### 3.1 Архитектура (классическая exo → embodiOS)

Классическая exo (exo-explore/ex-exo): P2P без master'а, модель режется по
слоям (`Shard{model_id, start_layer, end_layer, n_layers}`), UDP-broadcast
discovery (:5678, JSON каждые 2.5 с, таймаут 30 с), gRPC `NodeService`
(SendPrompt/SendTensor/SendResult/CollectTopology/HealthCheck), ring memory
weighted partitioning, OpenAI-совместимый API (:52415, SSE).

Маппинг на embodiOS (реализовано в skeleton этой ветки, `kernel/exo/`):

| exo (Python) | embodiOS (C) | Файл |
|---|---|---|
| `udp_discovery.py` | UDP :5678 JSON-анонсы поверх `tcpip_send_udp`/SOCK_DGRAM | `exo_discovery.c` |
| `grpc/node_service.proto` | бинарный TCP-протокол: 48-байтный `exo_tensor_msg_t` (magic/seq/dtype/dims/payload_len, network byte order) + payload | `exo_transport.c`, `exo.h` |
| `ring_memory_weighted_partitioning_strategy.py` | сортировка нод по `ram_free`, доли слоёв `mem/total`, кольцо | `exo_shard.c` |
| `orchestration/node.py` | state machine: recv TENSOR → свои слои → send next; хвост — sample+broadcast | `exo_node.c` (`exo__handle_tensor_msg`, `exo_forward_shard`) |
| `api/chatgpt_api.py` | минимальный HTTP: `POST /v1/chat/completions` (JSON/SSE), `/v1/models`, `/healthcheck` | `exo_server.c` |
| `inference/mlx,tinygrad` | существующий `streaming_inference.c` (точки стыковки — §3.3) | — |

### 3.2 Шаги и оценки (S/M/L)

| Шаг | Содержание | Объём | Статус в skeleton |
|---|---|---|---|
| B1 | TCP/IP + сокетный API | S (стек есть: net/tcpip.c) | ✓ используется |
| B2 | UDP-broadcast discovery (:5678, анонс `{node_id, ctrl_port, memory, memory_free, model}`, таблица пиров, таймаут 30 с, health) | M | ✓ `exo_discovery.c` (TX broadcast требует правки tcpip.c — TODO) |
| B3 | Бинарный протокол нод (PROMPT/TENSOR/RESULT/HEALTH/TOPOLOGY, length-prefixed) | M | ✓ `exo_transport.c` (чанкование ≤1400 из-за tx_buffer; reassembly/окна — ограничения стека) |
| B4 | Шард-загрузчик GGUF: мапить только `blk.[start..end)`, embedding на ring[0], lm_head на хвосте | M | ✗ TODO (каждая нода держит модель целиком; инференс-сторона готова — layer-range API) |
| B5 | Оркестратор: request_id → recv→слои→send; хвост: sample + broadcast RESULT + embed первой ноде | M–L | ✓ live: lockstep-кольцо на layer-range API (embed→forward_layers→TENSOR→…→sample→RESULT-барьер); одна нода = полный прогон через exo-путь |
| B6 | Ring memory weighted partitioning + rebalance при join/leave | M | ✓ `exo_shard.c` (rebalance дёргается из discovery) |
| B7 | OpenAI HTTP API (SSE) | M | ✓ упрощённо `exo_server.c` (наивный JSON; SSE-чанки) |
| B8 | Отказоустойчивость: таймауты, сброс request_id, перепартиция | M | ◐ таймаут нод есть; сброс запросов — TODO |
| B9 | Валидация: 2 ноды × GLM-4-9B vs одиночный прогон; затем 32B Q4_K_M на 2–4 нодах | M | ✗ после B4/B5 |

Итого фаза B: **L** (доминируют B5 и упрочнение транспорта).

### 3.3 Точки стыковки с существующим кодом (точные места)

**net/tcpip.c** (правки отдельной задачей, в skeleton — weak-стабы и TODO):
1. `tcpip_send_udp` (:712) и `tcp_send_packet` (:817): TX на 255.255.255.255
   должен идти eth-broadcast'ом без ARP — сейчас `NET_ERR_UNREACHABLE`.
2. `handle_ip` (:~561): принимать подсетевой broadcast
   `dst == (net_cfg.ip_addr | ~net_cfg.netmask)`.
3. `net_cfg` static (:16): нужны `tcpip_get_local_ip()/tcpip_get_local_mac()`
   (в exo объявлены weak-стабы — настоящие реализации заменят их при линковке).
4. `socket_recvfrom` для UDP (адрес отправителя сейчас читается через
   `tcpip_get_socket_for_testing()`).
5. Перспективно: MSS-сегментация TX, окно/rx_buffer >4096, reassembly —
   без этого throughput кольца ограничен (но hidden-вектор 12–24 КБ/токен
   проходит и так, ~9–17 чанков).

**ai/streaming_inference.c** (entry-функции выделены, ветка feat/exo-live):
1. ✅ Layer-range API: `streaming_inference_embed()` (token_embd lookup),
   `streaming_inference_forward_layers(hidden, pos, start, end)` (тело слоя
   вынесено в `layer_forward_stream()`; KV-cache индексируется глобальным
   номером слоя → шард трогает только свой KV-регион),
   `streaming_inference_sample_token()` (final norm + lm_head + temp/top-p),
   `streaming_inference_is_stop_token()`. `streaming_inference_generate()`
   переведён на те же helpers; регрессия в QEMU подтверждена
   (`chat` → «The capital of France is Paris.»).
2. Шард-маппинг весов: `MAP_LAYER_TENSOR`-цикл мапить только
   `[start,end)`; `token_embd` — только ring[0], `output/output_norm` —
   только хвост кольца (условно по `exo_shard_local()`). ✗ TODO (B4) —
   пока каждая нода мапит модель целиком.
3. ✅ Sampling на хвосте: `streaming_inference_sample_token()` вне цикла
   генерации; хвост кольца шлёт RESULT (token+finished) оркестратору.
   (:2291) в `int streaming_inference_sample_last(float *logits)`.
4. Embedding отдельно: `void streaming_inference_embed(int token, float *out)`
   (обёртка над `extract_embedding_transposed`, :1717) — первая нода шлёт
   embed нового токена по кольцу.

**core/stubs.c / core/kernel.c**: shell-команды `exo`, `exonodes`, `exoshard`,
`exoserve` (план в `kernel/exo/README.md`); `exo_poll()` — в `kernel_loop`
(core/kernel.c:371) рядом с `schedule()`.

### 3.4 Протокол тензоров (on-wire)

```
[exo_tensor_msg_t: 48 байт, network byte order]
  u32 magic = 0x45584F54 ("EXOT")   u16 version = 1   u16 msg_type
  u32 seq                            u64 request_id
  u32 dtype (0=f32,1=f16,2=bf16,3=i32)  u32 n_dims (<=4)  u32 dims[4]
  u32 payload_len
[payload: payload_len байт]
```

Сообщения: `PROMPT` (int32-токены), `TENSOR` (hidden-вектор токена,
f32/f16), `RESULT` (токен + is_finished — хвост кольца рассылает всем),
`HEALTH`, `TOPOLOGY` (gossip). На каждый токен по кольцу идёт только
hidden-вектор (32B: 6144·f16 ≈ 12 КБ × hop), KV-cache остаётся на
ноде-владельце слоёв.

---

## 4. Риски и нюансы

1. **Латентность hop'ов**, не полоса: ожидание ~10–20 ток/с для 9B-класса
   на 2–4 нодах по 1G Ethernet (оценка по бенчмаркам exo/llama.cpp TCP);
   текущий стек (один кадр на send, rx_buffer 4096, polling) добавляет
   накладные расходы — измерять на B9.
2. **RoPE**: GLM (оба arch) — partial rotary с **interleaved-парами**
   (NORM, не NeoX — уточнено по llama.cpp/HF, см. A4). Реализовано через
   `rope_n_dims` из `rope.dimension_count`; верифицировано на синтетике
   (A8.1).
3. **`attention_bias` различается** между GLM-4-9B-0414 (true) и 32B-0414
   (false) — загрузчик обязан терпеть оба.
4. **GLM-4-32B-0414: 61 слой** — не делится поровну; weighted partition это
   учитывает, но при равных нодах диапазоны будут 21/20/20.
5. **Стоп-токены**: фактический стоп диалога — `<|user|>` (151336), а не eos;
   обрабатывать все три (151329/151336/151338).
6. **Память ядра**: лимит 1 GB identity-map (kernel.c:232) блокирует любые
   модели >0.5B локально и KV-cache кольца — поднятие лимита (P1 аудита)
   является предпосылкой фаз A8/B9.
7. **Single-connection accept** в tcpip.c (:964, accept возвращает тот же
   fd): одновременные запросы к одной ноде сериализуются; для MVP приемлемо.
8. **ChatGLM3-6B** — SentencePiece (`chatglm-spm`), не GPT-2 BPE: если нужен,
   токенизатор — отдельная ветка работ.

## 5. Источники

- llama.cpp: `conversion/chatglm.py`, `conversion/glm.py`,
  `src/models/chatglm.cpp`, `src/models/glm4.cpp`, `src/llama-arch.cpp`,
  `src/llama-vocab.cpp`, `gguf-py/gguf/tensor_mapping.py`; PR #8031.
- HF: `zai-org/GLM-4-9B-0414`, `zai-org/GLM-4-32B-0414` (config.json),
  `bartowski/glm-4-9b-chat-GGUF`, `THUDM/glm-4-9b-chat-GGUF`,
  `ZhipuAI/glm-edge-1.5b-chat-gguf`.
- exo: `github.com/exo-explore/exo` (текущая MLX/RDMA-версия),
  `github.com/exo-explore/ex-exo` (классическая): `node_service.proto`,
  `exo/orchestration/node.py`, `exo/networking/udp/udp_discovery.py`,
  `exo/topology/ring_memory_weighted_partitioning_strategy.py`,
  `exo/api/chatgpt_api.py`.
- Аудит кодовой базы embodiOS: `audit.md` (состояние net-стека, движка,
  известные P0/P1).
