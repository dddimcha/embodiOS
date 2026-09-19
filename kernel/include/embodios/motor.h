/* EMBODIOS Real-Time Motor Control Demo
 *
 * 1 kHz closed-loop PID control of a simulated 2nd-order DC motor,
 * running on the rt_timer periodic callback layer (tick IRQ context).
 * The actuator output is one byte per control tick to IO port 0xE9
 * (QEMU isa-debugcon; harmless on real hardware). Callback jitter is
 * accounted cyclictest-style (rdtsc deltas vs the requested period).
 *
 * Commands (wired into the stubs.c dispatcher):
 *   motor run <ms> [hz]     run closed loop (default 5000 ms @ 1000 Hz),
 *                           then print the jitter report
 *   motor jitter            re-print the last jitter report
 *   motor llm on|off        LLM policy adjusts the setpoint asynchronously
 *                           (default off)
 *   motor plant dc|servo    plant model: dc = speed control (rad/s),
 *                           servo = position control (rad) (default dc)
 *   motor pid <kp> <ki> <kd>  set PID gains (no args: print current)
 */

#ifndef EMBODIOS_MOTOR_H
#define EMBODIOS_MOTOR_H

#ifdef __cplusplus
extern "C" {
#endif

void cmd_motor_dispatch(const char *args);

/* LLM policy bridge (implemented in core/stubs.c next to the chat
 * engine): runs one short generation and captures the reply text.
 * Task context only. Returns 0 on success, <0 when no model reply. */
int motor_llm_query(const char *prompt, char *out, unsigned long out_size);

#ifdef __cplusplus
}
#endif

#endif /* EMBODIOS_MOTOR_H */
