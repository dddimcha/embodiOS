# EMBODIOS v0.7.0 "Maxwell" — verification & benchmark report

Date: 2026-09-24. All in-guest measurements on QEMU 7.2 TCG (2-physical-core
sandbox host); host-side validation on lavapipe. TCG numbers are functional
evidence, not hardware performance claims.

## 1. Venus compute path (host-validated, hypervisor-gated)

Sandbox QEMU 7.2 ships no Venus/virgl device, so the gate is a host-side
harness (`tools/host_test_venus.c`, `make -C tools venustest`) that links the
**real kernel** `venus.c` against a mock virtio-gpu transport and a mock Venus
renderer implemented on lavapipe — the renderer consumes ring records exactly
like virglrenderer does.

- Wire format: real `VkCommandTypeEXT` ids from Mesa venus-protocol
  (vk.xml 1.4.357), record header `{u32 type, u32 flags}`, u64
  handles/array_sizes — field-for-field mirror of Mesa's generated
  `vn_encode_*`. 52 commands implemented (instance → ring management).
- Structural: 41-record init stream + 49-record matmul stream decode clean
  (cursor lands on record end, known opcodes, exact order asserted).
- Semantic: device bring-up replays through lavapipe entirely over the ring
  ("llvmpipe" device created via Venus records); matmul_f32 (3×64×5,
  16×512×9) and matmul_q4_k_q8_0 (3×256×5, 1×256×1, 4×512×7) are
  **bit-exact (max_abs_err = 0)** vs the CPU reference.
- In-guest: no Venus → `gpuinfo` prints "no virtio-gpu device", vk_device
  returns `VK_DEV_NO_DRIVER`, CPU/SIMD path untouched (smoke PASS).

Run on a Venus-capable hypervisor with:
`qemu-system-x86_64 -device virtio-gpu-gl,venus=on ...` (needs QEMU ≥7.1 +
virglrenderer built with Venus).

## 2. SMP-parallel quantized matmul (IPI worker pool)

- All fused matvecs in streaming_inference (Q8_0/Q4_K/Q5_0/Q6_K + 49152×576
  output head) row-partitioned across online CPUs; APs wake from `sti; hlt`
  via IPI per dispatch (`cpus` during chat: WorkItems/IPI-Wake ≈ 4868/AP,
  Polls = 0).
- **Bit-identity**: parbench FNV checksum `0x9D68488B` at 1/2/3/4 threads;
  chat answer "The capital of France is Paris." identical at -smp 1/2/4.
- Threshold: matrices < 131072 MACs stay serial (SmolLM attn k/v = 110k →
  serial; q/o 576×576, FFN 1536×576, head 49152×576 → parallel).

### Scaling (parbench, 1536×1536 Q4_K matvec)

| Host window | 1 CPU | 2 CPUs | 3 CPUs | 4 CPUs |
|---|---|---|---|---|
| uncontended | 43.5M cyc (1.00x) | 29.4M (1.47x) | 29.3M (1.48x) | 29.3M (1.48x) |
| contended   | 63.0M cyc (1.00x) | 62.7M (1.01x) | 64.3M (0.98x) | 65.6M (0.96x) |

The sandbox host has **2 physical cores**: 4 TCG vCPUs oversubscribe, so the
ceiling is the host, not the pool (the pre-existing smpwork integer demo
caps at 1.18x the same way). Per-dispatch cost is ~10k cycles (IPI+join)
against ≥131k MACs — on real hardware the math is firmly in the black.
UP (`-smp 1`) and `-append poll` paths unchanged (same tok/s as v0.6.0).

## 3. exo: live discovery, auto ring, 3-node demo

Topology: 3 QEMU nodes, `-netdev dgram` endpoints + userspace Python L2 hub
(QEMU 7.2 Debian lacks mcast netdev; naive round-robin hubbing caused ARP/SYN
backlog — the hub does select()+drain per frame).

```
nodeA/nodeB/nodeC — live UDP discovery (:5678), identical ring computed
on ALL nodes: [nodeA, nodeB, nodeC]  (ram_free desc, node_id tiebreak)
exoshard auto → 10/10/10 layers (smollm-30)
```

```
embodios> exochat 8 What is the capital of France?     # on orchestrator
B| [EXO] forward_shard: layers 10..20 pos=N len=576 -> 0
C| [EXO] forward_shard: layers 20..30 pos=N len=576 -> 0
[demo] Paris=yes transport_errors=none
[demo] RESULT: PASS    (8 tokens, 22 lockstep positions)
```

Failure handling (kill test `scripts/exo2node_kill.py`, node B SIGKILLed
mid-generation on the dgram hub):
- node A: RESULT timeout → clean abort with console error, ring marked
  **DEGRADED**, shell responsive — no hang, no panic, no triple-fault;
- B expires from A's table (~120 s) and the ring rebalances to 1 node;
- idle-kill rejoin proven (restart → rediscovery + ring reform in seconds);
- **Known issue**: rejoin *after a mid-generation kill* on the surviving
  orchestrator does not complete (guest RX stops seeing the restarted peer's
  beacons; TX/control fine) — suspected virtio-net RX-storm interaction,
  documented in `kernel/exo/README.md`.

Two-node regression (v0.6.0 harness, `exo2node.py`): PASS.

## 4. Standard gates (integrated build)

- `make test` 6/6 PASS · boot smoke → "Paris" · `-smp 1/2/4` identical
  answers · `-append poll` legacy mode PASS · UEFI gate (scripts/test_uefi.sh)
  7/7 PASS.

## Reproduce

```sh
make -C tools venustest                                   # Venus host gate
python3 gates_tesla2.py kernel/embodios.elf -smp 4        # then: parbench
python3 scripts/exo3node.py kernel/embodios.elf           # 3-node ring demo (~25 min TCG)
```
