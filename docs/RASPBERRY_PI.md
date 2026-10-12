# embodiOS on Raspberry Pi (v0.9.0 "Galvani")

Boot embodiOS on real Raspberry Pi hardware: one `kernel8.img` on a FAT32
SD card, and you land in the same preemptive LLM chat shell you know from
QEMU — on a board you can hold. This guide takes you from `git clone` to a
blinking cursor on a Pi 3, 4, or 5, and tells you exactly what to report
back so we can finish the hardware-validation milestone together.

---

## 1. Overview

The full embodiOS aarch64 kernel runs on the Pi, unmodified except for
compile-time board selection:

- **Preemptive priority scheduler** with the ARM Generic Timer at 100 Hz.
- **LLM chat** — SmolLM-135M (or any supported GGUF) embedded in the image
  when built with `GGUF_MODEL=...`; NEON fused Q4_K/Q8_0 kernels.
- **`simdtest`** — scalar-vs-NEON bit-exactness proof with FNV-1a-64 hashes
  that must be identical on x86_64, QEMU virt, and your Pi.
- **RT motor demo** — 100 Hz PID closed loop with jitter accounting and a
  UART trace.
- **Two consoles, mirrored**: the PL011 UART serial line (authoritative)
  **and** an HDMI framebuffer console (1024x768x32, 128x48 text cells,
  8x16 font) allocated through the VideoCore mailbox. Everything you see
  on serial also appears on the monitor, and either works for reading;
  input comes from serial.

Boot flow on real hardware: the Pi firmware (start*.elf / Pi 5 EEPROM)
loads `kernel8.img` at `0x80000`, enters at EL2 with the DTB pointer in
`x0`; `boot.S` saves the DTB pointer, drops EL2 → EL1h, sets up per-board
MMU device windows, and hands over to the common kernel.

## 2. Supported boards

Address matrix — the single source of truth is
[`kernel/arch/aarch64/board.h`](../kernel/arch/aarch64/board.h), verified
against the BCM2711 ARM Peripherals datasheet §6.5.1, the ARM GIC-400 TRM
§3.2 (GICD at +0x1000, GICC at +0x2000), and Linux `bcm2712.dtsi`:

| Board | SoC | PERIPH_BASE | UART0 (PL011) | GICD | GICC | MAILBOX |
|-------|-----|-------------|---------------|------|------|---------|
| Pi 3 (B/B+) | BCM2837 | `0x3F000000` | `0x3F201000` | `0x40001000` | `0x40002000` | `0x3F00B880` |
| Pi 4 (B) | BCM2711 | `0xFE000000` | `0xFE201000` | `0xFF841000` | `0xFF842000` | `0xFE00B880` |
| Pi 5 | BCM2712 + RP1 | `0x107C000000` | `0x107D001000` (debug UART) | `0x107FFF9000` | `0x107FFFA000` | `0x107C013880` |

All boards link at `0x00080000` (`kernel8.img` load address).

> **QEMU vs real hardware (Pi 3):** under `qemu-system-aarch64 -M raspi3b`
> there is no GIC-400 — QEMU models the Pi 3 with the ARM-local interrupt
> controller at `0x40000000`, and the build detects this at runtime
> (boot log: `INTC [pi3]: ARM-local intc, CNTPNS timer -> core0 IRQ`).
> The GIC-400 addresses in the table above are for **real Pi 3 hardware**.

## 3. Build & flash

### Build

```bash
# Pi 3:
make -C kernel ARCH=aarch64 BOARD=pi3 sdcard
# Pi 4:
make -C kernel ARCH=aarch64 BOARD=pi4 sdcard
# Pi 5:
make -C kernel ARCH=aarch64 BOARD=pi5 sdcard
```

(`BOARD` only exists for `ARCH=aarch64`; the cross toolchain is
`aarch64-linux-gnu-gcc` — see [QUICKSTART.md](QUICKSTART.md#prerequisites).)

**With the LLM embedded** (recommended — it's the point of the exercise):

```bash
./embodi pull smollm   # fetches models/smollm-135m-instruct-q4_k_m.gguf (~106 MB)
make -C kernel ARCH=aarch64 BOARD=pi4 sdcard
# or point at any supported GGUF explicitly:
make -C kernel ARCH=aarch64 BOARD=pi4 GGUF_MODEL=/path/to/smollm-135m-instruct-q4_k_m.gguf sdcard
```

- With a model, `kernel8.img` is **~106 MB** and `chat` works.
- Without `GGUF_MODEL` the image is small (a few MB) and everything except
  `chat` works — the shell tells you no model is embedded.

**Output:** `dist/sdcard/pi<N>/kernel8.img` + `dist/sdcard/pi<N>/config.txt`
(staged by [`scripts/make_sdcard.sh`](../scripts/make_sdcard.sh)).

> ⚠️ **CRITICAL — build hygiene.** After switching `BOARD`, `ARCH`, or
> `GGUF_MODEL`, **clean before rebuilding**:
>
> ```bash
> git clean -fdxq kernel/    # or use a fresh checkout
> ```
>
> The Makefile has no cross-ARCH/BOARD dependency tracking: stale object
> files from a previous configuration link into the new image silently and
> produce a **non-booting kernel (Prefetch Abort at/after boot)**. This is
> hard-won field experience — make `git clean -fdxq kernel/` a reflex
> between any two different configurations.

### Flash

**Easy path:** image an SD card with Raspberry Pi OS Imager (any Pi OS
Lite image is fine), then replace the kernel on the FAT32 boot partition
with ours:

```bash
cp dist/sdcard/pi4/kernel8.img /media/$USER/bootfs/
cp dist/sdcard/pi4/config.txt  /media/$USER/bootfs/
```

**Manual path (empty card):** MBR partition table, first partition FAT32.
Copy the official firmware blobs from
[`raspberrypi/firmware`](https://github.com/raspberrypi/firmware) (`boot/`)
plus our two files:

| Board | Firmware files to copy | Our files |
|-------|------------------------|-----------|
| Pi 3 | `bootcode.bin`, `start.elf`, `fixup.dat` | `kernel8.img`, `config.txt` |
| Pi 4 | `start4.elf`, `fixup4.dat` (no `bootcode.bin` — EEPROM bootloader) | `kernel8.img`, `config.txt` |
| Pi 5 | none — firmware is self-contained in the bootloader EEPROM; `config.txt` just has to exist for the partition to count as bootable | `kernel8.img`, `config.txt` |

(On Pi 5 the bootloader looks for `kernel_2712.img` first and falls back
to `kernel8.img`; our explicit `kernel=kernel8.img` line makes this
deterministic.)

**config.txt** (written by `make_sdcard.sh`; shown for pi4, which is the
superset):

```ini
# EMBODIOS pi4 boot configuration
arm_64bit=1
kernel=kernel8.img
enable_uart=1
arm_boost=1
disable_overscan=1
enable_gic=1
```

- `arm_64bit=1` — boot the ARM cores in AArch64 (implicit from the
  `kernel8.img` name, but we say it out loud).
- `kernel=kernel8.img` — load our image at `0x80000`.
- `enable_uart=1` — firmware enables the PL011 on GPIO14/15 and leaves it
  clocked.
- `arm_boost=1` — full turbo clock (Pi 4).
- `disable_overscan=1` — clean edges for the HDMI framebuffer console.
- `enable_gic=1` (**pi4 only**) — keeps the BCM2711's GIC-400 enabled
  instead of the legacy interrupt controller. Without it the GIC at
  `0xFF840000` is not usable from bare metal (BCM2711 ARM Peripherals
  §6.5.1). Harmless elsewhere; emitted only for pi4.

## 4. Wiring (UART)

You want a **USB-TTL 3.3 V** serial adapter (FTDI/CP2102/CH340 3.3 V —
**not 5 V**, the Pi GPIOs are not 5 V tolerant).

| Adapter | Pi header (all models with 40-pin header) |
|---------|-------------------------------------------|
| GND | Pin 6 (GND) |
| TXD (adapter) | Pin 10 = GPIO15 (RXD) |
| RXD (adapter) | Pin 8 = GPIO14 (TXD) |

Settings: **115200 8N1**, no flow control.

```bash
screen /dev/ttyUSB0 115200
# or: minicom -D /dev/ttyUSB0 -b 115200
```

**Pi 5:** prefer the dedicated **3-pin debug UART header** (between the
HDMI ports) — that's the PL011 at `0x107D001000` our build talks to.
Routing UART0 to GPIO14/15 on Pi 5 goes through the RP1 southbridge
(ALT4); the kernel programs those pins
(`kernel/drivers/gpio/bcm_gpio.c`) and the firmware option
`enable_rp1_uart=1` can pre-initialize RP1 UART0, but this path is **not
yet hardware-verified** — the debug header is the safe path.

**HDMI:** just plug in a monitor. The framebuffer console (1024x768x32,
128x48 text cells) mirrors all serial output from early boot — no
configuration needed.

## 5. Boot — what you should see

Annotated serial log from the QEMU raspi3b gate (model build); real
hardware prints the same lines with real firmware values:

```
MBOX: board=pi3 fw rev=0x000548e1 board model=0x00000000 uart clk=3000000 Hz (mailbox)
FB: 1024x768 pitch 4096 base 0x3c100000 size 3145728
EMBODIOS ARM64 booting...

=== EMBODIOS ARM64 ===
Bare-metal AI Operating System

Board: pi3 (PERIPH=0x3F000000 UART0=0x3F201000 GIC=0x40001000 LINK=0x00080000)
DTB:   0x0000000000000100
Exception Level: EL1
FPU/NEON: Enabled

INTC [pi3]: ARM-local intc, CNTPNS timer -> core0 IRQ

  ███████╗███╗   ███╗██████╗  ██████╗ ██████╗ ██╗ ██████╗ ███████╗
  ... ASCII logo ...
  EMBODIOS v0.9.0 · codename Galvani
  One binary. Any machine. No OS.

EMBODIOS Native Kernel v0.9.0
...
 [OK] GGUF model embedded (100 MB) — loads on first chat
 [OK] EMBODIOS Ready
Scheduler: Registered with timer for preemptive scheduling
Interrupts: ENABLED (generic timer PPI 30 @ 100 Hz, preemptive scheduling)
embodios>
```

Line by line:

- **`MBOX:`** — first proof the VideoCore mailbox works. Values above are
  QEMU's stub (fw rev `0x000548e1`; board model `0x0`; QEMU answers the
  UART-clock query with 3 MHz). On real firmware expect a real revision,
  a real board model, and a 48 MHz UART clock. If the mailbox doesn't
  answer at all, boot continues on the 48 MHz fallback divisor.
- **`FB:`** — framebuffer allocated through the mailbox: 1024x768, 32 bpp,
  pitch 4096. The base address differs per board/firmware (`0x3c100000`
  above is the QEMU value; bus address masked to ARM physical).
- **`Board:`** — the compile-time board identity and address matrix.
  Check it matches your board.
- **`DTB:`** — pointer the firmware/QEMU handed us in `x0` (parsing is
  future work; printing it proves the handoff).
- **`INTC [pi3]: ARM-local intc`** — only under QEMU raspi3b (no GIC-400
  in the emulation). On real Pi 3/4/5 hardware the GIC-400 driver
  initializes instead.
- **`embodios>`** — the cyan prompt. You're in.

## 6. Commands to try

```
help                                    # all commands
status                                  # kernel/version/uptime summary
chat What is the capital of France?     # streams tokens (model builds only)
simdtest                                # scalar vs NEON bit-parity + speed
motor run 2000 100                      # PID loop: 2000 ms @ 100 Hz, UART trace
motor jitter                            # scheduling jitter stats from the run
shutdown                                # see "Known issues" for what to expect
```

Expected results:

- **`chat`** — loads the embedded model on first use (tensor parsing
  progress bars), then streams the reply token by token with a final
  `(N tokens · prompt Xs · gen Ys · Z tok/s)` line.
- **`simdtest`** — must end in `simdtest: PASS — scalar and SIMD outputs
  are bit-identical` with these exact hashes:

  | Kernel | Hash (FNV-1a-64) |
  |--------|------------------|
  | Q8_0 × Q8_1 | `0xF648E55E42C8B617` |
  | Q4_K × Q8_1 | `0x87DFC84480B155A5` |

  These are **bit-exact across machines**: identical on x86_64, QEMU
  virt, QEMU raspi3b, and — this is the claim we need you to verify — your
  physical Pi. Matching hashes on real silicon are the cross-machine proof
  that the NEON kernels are correct.
- **`motor run 2000 100`** — prints `MOTOR: run 2000 ms @ 100 Hz,
  plant=dc setpoint=50.00 rad/s, pid=(6.00,10.00,0.02)` and finishes with
  a tick count. In the QEMU raspi3b gate it completed 40 control ticks
  (TCG time dilation); on real hardware expect ~200 ticks for 2000 ms at
  100 Hz. `motor jitter` then reports scheduling jitter statistics.
  `motor` with no arguments prints usage; also try `motor plant servo`,
  `motor llm on`.

## 7. Performance expectations

Pre-hardware numbers, measured under `qemu-system-aarch64` **TCG** (pure
software emulation) on a 2-core host, SmolLM-135M Q4_K_M, 8-token reply:

| Machine | tok/s | Source |
|---------|-------|--------|
| QEMU `-M virt` | 19.95–21.29 | v0.8.0 / v0.9.0 gates (run-to-run TCG variance) |
| QEMU `-M raspi3b` | 2.78 | v0.9.0 pi3 gate |

TCG single-threadedly emulates one vCPU, so raspi3b numbers are a floor,
not a ceiling: **real Pi 4/5 hardware will be significantly faster** than
TCG emulation, and real Pi 3 hardware should be comparable-or-better.
These are pre-hardware numbers awaiting your measurement — see §9.

## 8. Known issues & limitations

- **`shutdown` on real Pi hardware:** there is no PSCI under Pi firmware,
  so `shutdown`/`reboot` requests a reset through the VideoCore
  property mailbox / PM watchdog — **the board reboots** (it does not
  power off). Power-cycle the Pi to restart after that. Under QEMU
  raspi3b the watchdog reset does not exit QEMU either — kill QEMU
  manually (`Ctrl-A X`).
- **QEMU coverage:** QEMU 7.2 emulates only `raspi3b`. The pi4/pi5 builds
  are compile-clean and statically reviewed (address matrix verified
  against the datasheets above) but **await real-hardware validation** —
  that's your mission in §9.
- **Pi 5 GPIO14/15 UART:** the RP1 ALT4 path is programmed but unverified
  on hardware; use the 3-pin debug UART header (§4).
- **PMM reserve:** the kernel reserves the **top 64 MB of ARM-visible
  RAM** for the VideoCore (mailbox framebuffer / GPU heap, matching the
  default `gpu_mem=64` split). Usable RAM reported by the PMM is
  therefore ~64 MB less than the board's total — this is intentional and
  prevents framebuffer corruption.
- **SMP/network/exo** remain x86_64-only in this release and degrade
  gracefully on aarch64 (unchanged from v0.8.0).

## 9. Hardware test protocol — please report back!

Your run feeds directly into the v0.9.x validation milestones. Per board:

1. Build with the model (`GGUF_MODEL`), flash, wire UART, plug HDMI.
2. Capture the **full serial boot log** (screen/minicom logging on).
3. Run: `chat What is the capital of France?` → note the tok/s line.
4. Run: `simdtest` → note PASS/FAIL and both hashes.
5. Run: `motor run 2000 100` → note the tick count; then `motor jitter`.
6. Check the HDMI monitor shows the mirrored console (photo welcome!).
7. Try `shutdown` → confirm the board resets.

Copy-paste report template:

```
=== embodiOS v0.9.0 hardware report ===
Board:            (e.g. Raspberry Pi 4B, 4 GB / Pi 5 8 GB / Pi 3B 1 GB)
Firmware rev:     (from the "MBOX:" line)
Board line:       (from "Board: piN (PERIPH=...)")
FB line:          (from "FB: 1024x768 ...")
chat reply:       (first words + tok/s from the summary line)
simdtest:         PASS/FAIL · Q8_0 0x... · Q4_K 0x...
motor:            N control ticks @ 100 Hz · jitter mean/p99
HDMI mirror:      yes/no (photo attached: yes/no)
shutdown:         reset observed: yes/no
Anything odd:     (garbled serial, hangs, missing lines, EL at entry, ...)
```

Open an issue (or reply on the release thread) with the filled template.
A Pi 4 or Pi 5 report with matching simdtest hashes closes the v0.9.0
hardware-validation gate.

## 10. QEMU pre-flight (no hardware needed)

Rehearse the exact Pi 3 boot path under emulation — same `kernel8.img`,
same `board.h` addresses, same mailbox code (QEMU stubs it):

```bash
make -C kernel ARCH=aarch64 BOARD=pi3 sdcard   # pi3 build only
qemu-system-aarch64 -M raspi3b -m 1G \
    -kernel dist/sdcard/pi3/kernel8.img \
    -serial stdio -display none
```

You should see the boot sequence from §5, including the `MBOX:` and `FB:`
lines (QEMU's framebuffer is emulated; `-display none` keeps it serial-only).
This is the configuration the v0.9.0 gate ran: boot, chat, simdtest,
motor — all PASS. Exit with `Ctrl-A X`. Only the pi3 build works here;
QEMU 7.2 has no raspi4b/raspi5b machine.
