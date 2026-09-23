/* EMBODIOS x86_64 Local APIC Timer
 *
 * Calibrated periodic tick source that replaces the legacy 100 Hz PIT
 * tick with a LAPIC timer running at a higher rate (default 1000 Hz).
 * The LAPIC timer delivers on the legacy IRQ0 vector (0x20), so the
 * existing IDT stub and tick chain are reused; when active, the PIT line
 * stays masked at the PIC and the tick EOI goes to the LAPIC.
 *
 * Falls back transparently: if probe or init fails, the caller keeps the
 * PIT path unchanged (lapic_timer_active() == 0).
 */

#ifndef EMBODIOS_LAPIC_TIMER_H
#define EMBODIOS_LAPIC_TIMER_H

#include <embodios/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if the CPU has a usable local APIC (CPUID.1:EDX.APIC + IA32_APIC_BASE) */
int      lapic_timer_probe(void);

/* Calibrate the LAPIC bus clock against the HPET main counter (fallback:
 * PIT channel 2 one-shot), then program the LVT timer in periodic mode.
 * Returns 0 on success, <0 on failure (caller stays on the PIT). */
int      lapic_timer_init(uint32_t hz);

/* Programmed tick rate (valid after successful init) */
uint32_t lapic_timer_hz(void);

/* Monotonic LAPIC tick count since init */
uint64_t lapic_timer_ticks(void);

/* LAPIC EOI; replaces the PIC EOI for the tick vector when active */
void     lapic_timer_eoi(void);

/* 1 when the LAPIC timer is the active tick source */
int      lapic_timer_active(void);

/* Tick IRQ entry (called from the IRQ0-vector handler in idt.c):
 * advances the tick counter and dispatches the rt_timer callback layer. */
void     lapic_timer_tick(void);

/* Returns non-zero when the decimated legacy 100 Hz tick chain
 * (timer_tick/timer_interrupt_handler -> scheduler_tick) is due, so the
 * scheduler quantum, uptime and hal_timer semantics stay unchanged. */
int      lapic_timer_legacy_due(void);

/* Calibration source actually used: "HPET" or "PIT" (valid after init) */
const char *lapic_timer_calib_source(void);

/* --- WS-A: per-CPU LAPIC timers on APs (IPI-based parking) --- */

/* Local-tick vector for AP LAPIC timers (distinct from the BSP's 0x20:
 * an AP tick must never enter the BSP scheduler/tick chain in idt.c) */
#define LAPIC_AP_TICK_VECTOR  0xF1

/* Calibrate and arm the calling CPU's LAPIC timer in periodic mode on
 * LAPIC_AP_TICK_VECTOR. For APs only (called via ipi_init_ap during
 * AP bring-up, before the BSP's own calibration exists): measures the
 * LAPIC bus clock against the HPET (PIT channel 2 fallback) with the
 * same helpers as the BSP path, reusing an already-measured bus-clock
 * base when available. Programs only the local CPU's LVT timer; the
 * global BSP tick state (tick_count, legacy decimation) is untouched.
 * Returns 0 on success, <0 on failure (caller falls back to polling). */
int      lapic_timer_init_ap(uint32_t hz);

/* Measured LAPIC bus clock in Hz (0 before any successful calibration) */
uint32_t lapic_timer_bus_hz(void);

/* AP local-tick IRQ entry (called from interrupt_handler in idt.c for
 * LAPIC_AP_TICK_VECTOR): LAPIC EOI + per-CPU tick accounting only. */
void     lapic_timer_ap_tick(void);

/* Per-CPU local tick count by APIC ID (diagnostics / liveness) */
uint64_t lapic_timer_ap_ticks(uint32_t apic_id);

#ifdef __cplusplus
}
#endif

#endif /* EMBODIOS_LAPIC_TIMER_H */
