/* EMBODIOS x86_64 Inter-Processor Interrupts (IPI)
 *
 * Cross-CPU interrupt delivery over the Local APIC ICR, plus the per-CPU
 * wakeup path that lets APs park in `sti; hlt` (IF=1) instead of the
 * legacy IF=0 pause-polling mailbox loop:
 *
 *   - ipi_send(cpu, vec): physical-destination fixed IPI to one CPU
 *     (APIC IDs come from the smp.c topology tables).
 *   - ipi_init_ap(): per-AP setup — calibrates and arms the AP's own
 *     LAPIC timer (lapic_timer_init_ap) so the AP has a local periodic
 *     tick and can sleep with interrupts enabled.
 *   - IPI_WAKEUP_VECTOR (0xF0): delivered when work is posted to an AP
 *     mailbox; the handler only EOIs and counts — the actual work runs
 *     in the AP's normal (non-IRQ) park loop after hlt wakes.
 *   - Vector 0xFF is the LAPIC spurious vector; its handler must NOT
 *     send an EOI (spurious interrupts need none).
 *
 * UP boots (-smp 1) never touch this file's send path: smp.c skips the
 * APIC entirely and no IPI is ever issued.
 */

#ifndef EMBODIOS_IPI_H
#define EMBODIOS_IPI_H

#include <embodios/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-IPI vector used to wake a parked AP when mailbox work is posted */
#define IPI_WAKEUP_VECTOR  0xF0

/* Periodic local-tick rate armed on each AP by ipi_init_ap() */
#define IPI_AP_TICK_HZ     100

/* Send a fixed-mode, physical-destination IPI to a CPU (sequential id).
 * Returns 0 on success, <0 if the target/LAPIC is unavailable (caller
 * falls back to mailbox polling). Never sends to the calling CPU's
 * mailbox targets on UP boots — smp.c rejects those before calling. */
int  ipi_send(uint32_t cpu, uint8_t vector);

/* Per-AP IPI/timer setup, called from smp_ap_main after the LAPIC is
 * enabled: calibrates and arms this AP's LAPIC timer (periodic, on
 * LAPIC_AP_TICK_VECTOR) using the lapic_timer.c calibration helpers.
 * Returns 0 when the AP can park with IF=1 (IPI + local timer live),
 * <0 when the AP must fall back to the IF=0 polling loop. */
int  ipi_init_ap(void);

/* Wakeup-vector handler (called from interrupt_handler in idt.c):
 * LAPIC EOI + wakeup accounting only; mailbox work itself runs in the
 * AP park loop outside IRQ context (SSE/FPU-safe). */
void ipi_wakeup_handler(void);

/* Wakeup IPIs delivered to a CPU (by sequential cpu id; 0 if unknown) */
uint64_t ipi_wakeup_count(uint32_t cpu);

/* Spurious vector (0xFF) handler: LAPIC spurious interrupts require no
 * EOI — just count and return. */
void ipi_spurious_handler(void);

/* Total spurious interrupts seen (diagnostic) */
uint64_t ipi_spurious_count(void);

#ifdef __cplusplus
}
#endif

#endif /* EMBODIOS_IPI_H */
