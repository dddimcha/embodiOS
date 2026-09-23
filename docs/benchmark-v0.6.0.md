# EMBODIOS v0.6.0 "Volta" — verification & benchmark report

Date: 2026-09-23. All measurements on QEMU 7.2 (TCG, software CPU) inside a
Linux sandbox. TCG numbers are **functional** evidence, not hardware
performance claims — expect large speedups on KVM/HVF.

## 1. SMP: per-CPU LAPIC timers + IPI wakeup

Boot, `-smp 4`:

```
SMP: Detected 4 CPU(s)
SMP: AP cpu 1 (APIC ID 1) online, stack 0x780C020, parking sti;hlt IF=1 (IPI wakeup)
SMP: AP cpu 2 (APIC ID 2) online, stack 0x781C040, parking sti;hlt IF=1 (IPI wakeup)
SMP: AP cpu 3 (APIC ID 3) online, stack 0x782C060, parking sti;hlt IF=1 (IPI wakeup)
SMP: Initialization complete (4 CPUs detected, 4 online)
TICK: LAPIC timer @1000 Hz (calibrated vs HPET)
```

`cpus` after boot (steady state):

```
CPUs: 4 detected, 4 online
CPU  APIC ID  Role   State          WorkItems  IPI-Wake AP-Ticks Polls
1    1        AP     parked IF=1    0          0        40       0
2    2        AP     parked IF=1    0          0        40       0
3    3        AP     parked IF=1    0          0        37       0
```

- APs park in `sti; hlt` with **IF=1** (interrupt-halt), woken by IPI —
  the IF=0 polling mailbox is gone (`Polls` stays 0).
- Each AP runs its own HPET-calibrated LAPIC timer (`AP-Ticks` counting).

`smpwork` (2,000,000 iterations × 4 CPUs, rdtsc cycles):

```
CPU 0 (BSP): 106601694 cycles  result=0x7E038E1299B321F4
CPU 1 (AP):  19894414 cycles  result=0x4CD5C7FE204A6E24
CPU 2 (AP):  19704326 cycles  result=0x528A191F4F6719B7
CPU 3 (AP):  19460296 cycles  result=0xDF3AB52FB0862462
```

(BSP absorbs console/IRQ traffic under TCG; all four cores execute work.)

Regression gates: `-smp 1` UP mode skips APIC init and boots normally;
`-append poll` forces legacy polling mode (`Interrupts: DISABLED (cmdline
'poll' -> legacy polling mode)`) and the shell remains functional.

## 2. GPU compute: SPIR-V shader coverage

| Shader            | Words  | Verification                        |
|-------------------|--------|-------------------------------------|
| matmul_f32        | (v0.5) | host test, v0.5.0                   |
| matmul_q8_0       | (v0.5) | in-shader dequant, v0.5.0           |
| matmul_q4_k_q8_0  | 1599   | **bit-exact** vs CPU ref (lavapipe) |
| matmul_q6_k_q8_0  | 1759   | **bit-exact** vs CPU ref (lavapipe) |

Q4_K/Q6_K are hand-assembled by `tools/spirv_gen.py` (no shader compiler
in-tree) and dispatched by `ai/vk_device.c`; the device layer returns
`VK_DEV_NO_DRIVER` until a GPU transport is present, so CPU inference is
unaffected. This covers the full quantization set of the verified models
(Qwen2.5-1.5B uses Q4_K + Q6_K).

## 3. virtio-gpu transport + Venus

- Modern virtio-gpu probe via vendor-capability MMIO; legacy transitional
  device (PCI 0x1010) fallback.
- Control + cursor virtqueues, `GET_DISPLAY_INFO`, capset enumeration.
- Venus capset (id 4) layer spec-complete — the transport the Vulkan
  device layer will use on Venus-capable hypervisors.
- New shell command: `gpuinfo`.

## 4. Model verification (bare metal, models embedded in the kernel ELF)

| Model | Size | Arch | Result |
|-------|------|------|--------|
| SmolLM-135M-Instruct Q4_K_M | ~110 MB | llama | "The capital of France is Paris." |
| Qwen2.5-1.5B-Instruct Q4_K_M | 1.12 GB | qwen2 (+attn QKV bias) | loads in **153.9 s** → "Paris" |
| GLM-Edge-1.5B-Chat Q4_K_M | ~1.0 GB | glm | **zero code changes** → "Paris" |

The Qwen2.5 blocker was boot-time, not inference: with >1 GiB embedded,
`.bss` lands above the 1 GiB identity map and the first stack access after
the far jump triple-faulted. `boot.S` now identity-maps 4 GiB
(`BOOT_IDENTITY_GB = 4`). Details: `docs/models.md`.

Reference inference speed (SmolLM-135M, `-smp 1`, TCG, integrated build):
prompt eval 8.1 s, generation **1.22 tok/s** (50 tokens). Multi-instance
TCG contention reduces this (0.69 tok/s observed with 3 VMs running).

## 5. Direct UEFI boot (no GRUB)

`make uefi` → `kernel/BOOTX64.EFI` (PE32+ emitted by `tools/mkuefi.py`) +
`kernel/esp.img` (FAT16 ESP via `tools/mkesp.py`). Booted under OVMF
pflash:

- `scripts/test_uefi.sh`: **7/7 PASS** (ESP structure, PE32+ headers,
  boot to shell, kernel cmdline handoff).
- UEFI chat smoke: "What is the capital of France?" → "Paris",
  **1.38 tok/s**.

Design: loader uses the Simple File System protocol, `AllocatePages`,
parses ELF64, synthesizes a multiboot2 info block at 0x9000 (cmdline
type-1 + mmap type-6 tags), `ExitBootServices`, drops 64→32 bit, jumps to
`_start`. Docs: `docs/uefi-boot.md`.

## 6. exo distributed inference over a real TCP network

Two QEMU instances, socket netdev pair (distinct MACs), 10.0.0.1/30:

```
nodeA (3072 MB, ring[0]): layers 0..15    nodeB (2048 MB, ring[1]): layers 15..30
```

Deterministic sharding via `exoshard smollm 30 even` (identical ring
tables on both nodes). Ring generation triggered from the serial console
with the new `exochat` command:

```
embodios> exochat 8 What is the capital of France?
[EXO] RESULT req=1 seq=21 token=30 finished=0
[EXO] RESULT req=1 seq=22 token=2 finished=1
[EXO] ring generate done: 8 tokens (22 pos)
The capital of France is Paris.
```

- 8 tokens / 22 lockstep positions across the ring, **zero transport
  failures** (persistent connections, one connection per peer per
  direction).
- Ring numerics match local inference exactly: same prompt → identical
  output on the ring path and on the single-node `chat` control.
- Debugging aids added: `tcpsockets` (socket table dump), exo transport
  diagnostic prints.

### TCP hardening — five bugs found and fixed by this demo

1. No SYN retransmission (first SYN dropped on ARP miss) → 500 ms × 8
   retry in `tcpip_check_timeouts()`.
2. TIME_WAIT never expired for `timeout_ms=0` sockets → socket exhaustion
   → 1 s expiry.
3. virtio_net TX completion 100 ms → 2000 ms (TCG vCPU stalls surfaced as
   `VIRTIO_ERR_TIMEOUT` on RESULT sends).
4. Connect-per-token churn → persistent connections (header-magic races
   eliminated).
5. `SOCKET_BUFFER_SIZE` 4096 → 16384 (8 KiB hidden vector + 48 B header).

## Reproduce

```sh
# SMP gate
python3 gates_tesla2.py kernel/embodios.elf -smp 4     # cpus / smpwork / tasktest
# UEFI gate
make -C kernel uefi && scripts/test_uefi.sh
# Two-node ring demo
python3 exo2node.py kernel/embodios.elf "What is the capital of France?"
```
