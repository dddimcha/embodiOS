/* EMBODIOS Real-Time Periodic Callback Layer
 *
 * Single list of up to 8 periodic callbacks dispatched from the system
 * tick IRQ (rt_timer_poll). Callbacks run in IRQ context on the BSP:
 * they must not sleep, allocate memory, or touch the console; FPU/SSE
 * state of the interrupted context is preserved around each invocation.
 *
 * The poll timebase is the fast tick: 1000 Hz when the LAPIC timer is
 * active, 100 Hz on the PIT fallback (see rt_timer_set_tick_hz).
 */

#ifndef EMBODIOS_RT_TIMER_H
#define EMBODIOS_RT_TIMER_H

#include <embodios/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*rt_timer_fn)(uint64_t tick, void *arg);

/* Register a periodic callback. period_us is rounded up to whole tick
 * periods (requests faster than the tick rate clamp to every tick).
 * Returns 0 on success, <0 when the list is full or args are bad. */
int  rt_timer_register(rt_timer_fn fn, uint32_t period_us, void *arg);

/* Remove all registrations for fn (safe to call when not registered). */
void rt_timer_unregister(rt_timer_fn fn);

/* Tick IRQ entry: dispatch due callbacks. now_ticks is monotonic in
 * units of the current tick rate. */
void rt_timer_poll(uint64_t now_ticks);

/* Poll timebase rate in Hz (set by the tick source at enable time) */
void     rt_timer_set_tick_hz(uint32_t hz);
uint32_t rt_timer_get_tick_hz(void);

/* Callback jitter instrumentation: while armed, every callback invocation
 * records its lateness (actual period - requested period, rdtsc-based,
 * clamped to >= 0) in microseconds into a ring of 8192 u32 samples.
 * rt_timer_jitter_copy() takes an irq-safe snapshot (oldest first). */
void     rt_timer_jitter_arm(void);
void     rt_timer_jitter_disarm(void);
uint32_t rt_timer_jitter_samples(void);
uint32_t rt_timer_jitter_copy(uint32_t *dst, uint32_t max);

#ifdef __cplusplus
}
#endif

#endif /* EMBODIOS_RT_TIMER_H */
