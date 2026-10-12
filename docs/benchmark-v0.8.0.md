# EMBODIOS v0.8.0 "Faraday" — verification & benchmark report

Date: 2026-10-12. All aarch64 measurements in-guest on QEMU TCG
(`qemu-system-aarch64 -M virt -cpu cortex-a53 -m 2G -nographic`, software
CPU emulation); x86_64 regression gates on QEMU x86_64 TCG. TCG numbers
are functional evidence, not hardware performance claims.

## 1. aarch64 boot & platform gates

First release to boot on ARM64. EL1 entry from `boot.S`, console on PL011
(`0x09000000`, polling input), 16-entry exception vector table via
`VBAR_EL1`, GICv2 (GICD `0x08000000` / GICC `0x08010000`) with the ARM
Generic Timer PPI (INTID 30) at 100 Hz and UART RX on INTID 33.
`rt_timer` fires in IRQ context; PSCI (`hvc #0`) provides
SYSTEM_OFF/SYSTEM_RESET.

| Gate | Result |
|---|---|
| boot + shell | **PASS** — banner, `embodios>` prompt, `help`/`status` |
| chat | **PASS** — "The capital of France is Paris.", 8 tokens, **19.95 tok/s** (TCG) |
| shutdown | **PASS** — PSCI SYSTEM_OFF, QEMU exits rc=0 |

Preemptive scheduling is real on aarch64: full x19–x30 callee-saved
context switch, `tasktest` PASS with 21 context switches, timer-driven
preemption from the 100 Hz Generic Timer IRQ.

## 2. NEON dispatch & `simdtest` bit-parity

Q4_K/Q8_0 fused `vec_dot` dispatch to real NEON kernels on aarch64 (they
were scalar aliases before). Three kernel bugs found and fixed by the
parity gate: fp16 bit-reinterpretation, tail guard when `cols % 8 != 0`,
FIXED_SHIFT scaling.

New shell command `simdtest`: runs scalar and SIMD fused `vec_dot` over
the same inputs, FNV-1a-64-hashes the outputs, and times both paths.

```
embodios> simdtest
Q8_0  scalar   : hash=0xF648E55E42C8B617  ticks/pass=5460
      NEON     : hash=0xF648E55E42C8B617  ticks/pass=5656  PASS
      NEON(exp): hash=0xF648E55E42C8B617  ticks/pass=5543  PASS
Q4_K  scalar   : hash=0x87DFC84480B155A5  ticks/pass=8704
      NEON     : hash=0x87DFC84480B155A5  ticks/pass=5827  PASS
      NEON(exp): hash=0x87DFC84480B155A5  ticks/pass=5784  PASS
simdtest: PASS — scalar and SIMD outputs are bit-identical
```

Hashes are bit-identical scalar vs NEON on both quant formats. Measured
speedup under TCG: Q4_K NEON **1.49x** (5827 vs 8704 ticks/pass); Q8_0
is parity-bound under TCG (5460 vs 5656 — TCG translates NEON at scalar
cost). On real Cortex-A silicon the NEON gap widens in favor of SIMD.

## 3. motor RT demo + LLM policy on aarch64

The 100 Hz PID closed loop (`rt_timer` IRQ callbacks, jitter accounting,
asynchronous LLM policy hook) runs unmodified on ARM64:

```
embodios> motor llm on
MOTOR: LLM policy hook ON (setpoint adjusts during 'motor run')
embodios> motor run 2000 1000
MOTOR: run complete — 199 control ticks @ 100 Hz
embodios> motor jitter
JITTER: count=198 mean=132.48 us stddev=502.91 us min=0 us max=5773 us p99=2607 us
```

**PASS.** Mean jitter 132 µs / p99 2607 µs under TCG with the LLM hook
active — the same gate shape as the x86_64 motor demo.

## 4. x86_64 regression gates

- `make test` **6/6 PASS**.
- Boot smoke **PASS** — "The capital of France is Paris.", 8 tokens.
- Two-node exo ring **PASS** — 15+15 layer shard, 8 tokens over 21
  lockstep positions, 0 transport errors (`v080_x86_exo2node.log`).

## Out of scope on aarch64

exo distributed inference, the TCP/IP network stack, and SMP remain
x86_64-only in v0.8.0; their commands degrade gracefully on aarch64.
QEMU emulates only the `virt` machine — not Raspberry Pi. BCM2712
drivers (Pi 5 GPIO/I2C/SPI) are already in the repo; the hardware phase
(RP1 GIC remap, `kernel8.img`, mailbox clocks) is the next release.

## Reproduce

```sh
# cross toolchain: aarch64-linux-gnu-gcc + qemu-system-aarch64
make -C kernel ARCH=aarch64 GGUF_MODEL=/path/to/model.gguf
qemu-system-aarch64 -M virt -cpu cortex-a53 -m 2G -nographic \
    -kernel kernel/embodios.elf
# in the shell: simdtest · motor run 2000 1000 · chat <prompt> · shutdown
```
