/* EMBODIOS EMG shell commands (SPEC-M0, WS-DSP)
 *
 *   emg                live state dump (envelope, baseline, MVC, class,
 *                      level, latency/jitter, estop)
 *   emgcal             calibration sequence (2 s rest baseline, 2 s MVC)
 *   emgtest synthetic  deterministic scripted run: 1 s rest -> 1.5 s
 *                      contraction ramp -> 1 s hold -> release; prints the
 *                      REST/CONTRACT transition log with timestamps and the
 *                      measured detection latency
 *   m0demo             full bench loop: rt tick @ 1 kHz (period clamps to
 *                      the platform tick), 10 s synthetic contraction
 *                      window, live trace, then estop + disable
 *
 * All pipeline runs here (except the m0demo rt loop) are synchronous in
 * task context, so console output is allowed. The rt tick itself never
 * prints (IRQ context); m0demo polls state snapshots from task context.
 */

#include <embodios/emg.h>
#include <embodios/actuator.h>
#include <embodios/console.h>
#include <embodios/rt_timer.h>
#include <embodios/hal_timer.h>
#include <embodios/tsc.h>
#include <embodios/types.h>
#include <string.h>

/* Internal EMG-module helpers (core/emg/emg.c, dsp.c, classify.c) */
extern void     emg_dsp_reset(int ch);
extern void     emg_dsp_warmup(int samples);
extern void     emg_classify_reset(void);
extern void     emg_synth_reset(void);
extern void     emg_synth_set_mode(int mode);
extern int      emg_synth_read(int ch);
extern int      emg_using_synth(void);
extern int      emg_rt_is_active(void);
extern uint32_t emg_act_trace_copy(uint32_t *ticks, float *levels, uint32_t max);
extern struct emg_state *emg_state_mut(void);

enum { SYNTH_IDLE = 0, SYNTH_BURST, SYNTH_SCRIPT, SYNTH_DEMO };

#define ACT_TRACE_MAX 32    /* matches ACT_TRACE_RING in emg.c */

/* Mirrors the emgtest timeline in emg.c: 1000 rest + 1500 ramp +
 * 1000 hold + 500 release tail, in samples @ 1 kHz. */
#define EMGTEST_SCRIPT_SAMPLES 4000
#define EMGTEST_BURST_START_MS 1000
#define EMGTEST_RELEASE_MS     3500

#if defined(__aarch64__)
static inline void emg_cpu_relax(void) { __asm__ volatile("yield" ::: "memory"); }
#else
static inline void emg_cpu_relax(void) { __asm__ volatile("pause" ::: "memory"); }
#endif

static uint64_t emg_now_ms(void)
{
    return hal_timer_get_milliseconds();
}

static const char *cls_name(emg_class_t c)
{
    return c == EMG_CONTRACT ? "CONTRACT" : "REST";
}

/* ============================================================================
 * emg — live state dump
 * ============================================================================ */

static void cmd_emg_status(void)
{
    const struct emg_state *st = emg_get_state();

    console_printf("EMG: source=%s actuator=%s rt=%s\n",
                   emg_using_synth() ? "synthetic (no EMG hardware)" : "hardware",
                   actuator_name(),
                   emg_rt_is_active() ? "running" : "stopped");
    console_printf("EMG: channels=%d calibrated=%s class=%s level=%f estop=%s\n",
                   st->channels,
                   st->calibrated ? "yes" : "no (defaults)",
                   cls_name(st->cls), (double)st->level,
                   st->estop ? "LATCHED" : "no");
    for (int ch = 0; ch < st->channels && ch < EMG_MAX_CHANNELS; ch++) {
        console_printf("EMG: ch%d envelope=%f baseline=%f mvc=%f\n",
                       ch, (double)st->envelope[ch],
                       (double)st->baseline[ch], (double)st->mvc[ch]);
    }
    console_printf("EMG: tick=%u last_latency=%u us jitter_max=%u us\n",
                   st->tick, st->last_latency_us, st->jitter_us_max);
}

/* ============================================================================
 * emgtest synthetic — scripted run with transition log
 * ============================================================================ */

static int cmd_emgtest_synthetic(void)
{
    const struct emg_state *st = emg_get_state();

    if (!emg_using_synth()) {
        console_printf("EMGTEST: hardware source present — "
                       "'emgtest synthetic' requires the synthetic source\n");
        return -1;
    }

    console_printf("EMGTEST: synthetic scripted run "
                   "(1s rest -> 1.5s ramp -> 1s hold -> release)\n");

    /* Calibrate on the synthetic source so thresholds are real */
    if (emg_calibrate() != 0) {
        console_printf("EMGTEST: FAIL (calibration)\n");
        return -1;
    }

    /* Scripted run, synchronous (1 sample = 1 ms simulated) */
    emg_synth_reset();
    emg_synth_set_mode(SYNTH_SCRIPT);
    for (int ch = 0; ch < EMG_MAX_CHANNELS; ch++) {
        emg_dsp_reset(ch);
    }
    emg_classify_reset();
    emg_dsp_warmup(300);    /* settle HP transient, restart script at t=0 */

    struct emg_state *mst = emg_state_mut();
    emg_class_t prev = EMG_REST;
    int transitions = 0;
    int on_latency = -1, off_latency = -1;
    uint64_t proc_us_total = 0, proc_us_max = 0;

    for (int i = 0; i < EMGTEST_SCRIPT_SAMPLES; i++) {
        uint64_t t0 = rdtsc();
        for (int ch = 0; ch < mst->channels; ch++) {
            mst->envelope[ch] = emg_dsp_process(ch, (float)emg_synth_read(ch));
        }
        emg_classify_tick();
        uint64_t dt = tsc_to_microseconds(rdtsc() - t0);
        proc_us_total += dt;
        if (dt > proc_us_max) proc_us_max = dt;

        if (mst->cls != prev) {
            console_printf("EMGTEST: t=%d ms %s -> %s (env=%f level=%f)\n",
                           i, cls_name(prev), cls_name(mst->cls),
                           (double)mst->envelope[0], (double)mst->level);
            if (prev == EMG_REST && on_latency < 0) {
                on_latency = i - EMGTEST_BURST_START_MS;
            } else if (prev == EMG_CONTRACT && off_latency < 0) {
                off_latency = i - EMGTEST_RELEASE_MS;
            }
            prev = mst->cls;
            transitions++;
        }
    }

    emg_synth_set_mode(SYNTH_IDLE);

    console_printf("EMGTEST: transitions=%d (expect 2)\n", transitions);
    console_printf("EMGTEST: onset latency=%d ms after burst start "
                   "(includes the 1.5s ramp up to the 25%% threshold)\n",
                   on_latency);
    console_printf("EMGTEST: release latency=%d ms after release "
                   "(budget 100 ms)\n", off_latency);
    console_printf("EMGTEST: pipeline cost mean=%llu us max=%llu us per sample\n",
                   (unsigned long long)(proc_us_total / EMGTEST_SCRIPT_SAMPLES),
                   (unsigned long long)proc_us_max);
    console_printf("EMGTEST: final state class=%s estop=%s\n",
                   cls_name(st->cls), st->estop ? "LATCHED" : "no");

    int pass = (transitions == 2 && on_latency >= 0 &&
                off_latency >= 0 && off_latency < 100 && !st->estop);
    console_printf("EMGTEST: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : -1;
}

/* ============================================================================
 * m0demo — 10 s rt bench loop, then estop
 * ============================================================================ */

#define M0DEMO_MS 10000

static int cmd_m0demo(void)
{
    const struct emg_state *st = emg_get_state();
    struct emg_state *mst = emg_state_mut();

    console_printf("M0DEMO: EMG->actuator bench loop, %u s, source=%s actuator=%s\n",
                   M0DEMO_MS / 1000,
                   emg_using_synth() ? "synthetic" : "hardware",
                   actuator_name());

    if (!st->calibrated) {
        console_printf("M0DEMO: not calibrated — running calibration first\n");
        if (emg_calibrate() != 0) {
            console_printf("M0DEMO: FAIL (calibration)\n");
            return -1;
        }
    }

    actuator_init();    /* clears a latched estop (SPEC: re-enable only here) */
    mst->estop = 0;

    emg_synth_reset();
    emg_synth_set_mode(SYNTH_DEMO);
    emg_rt_enable(1);
    if (!emg_rt_is_active()) {
        console_printf("M0DEMO: FAIL (rt enable)\n");
        return -1;
    }
    console_printf("M0DEMO: rt loop running @ %u Hz tick "
                   "(contraction window 2s..7s)\n", rt_timer_get_tick_hz());

    uint64_t start = emg_now_ms();
    uint64_t last_print = 0;
    while (emg_now_ms() - start < M0DEMO_MS) {
        uint64_t now = emg_now_ms() - start;
        if (now - last_print >= 1000) {
            last_print = now;
            console_printf("M0DEMO: t=%llu ms tick=%u env0=%f level=%f class=%s%s\n",
                           (unsigned long long)now, st->tick,
                           (double)st->envelope[0], (double)st->level,
                           cls_name(st->cls),
                           st->estop ? " ESTOP" : "");
        }
        emg_cpu_relax();
    }

    /* End of demo: estop + disable (SPEC-M0) */
    actuator_estop();
    mst->estop = 1;
    emg_rt_enable(0);

    console_printf("M0DEMO: done — %u ticks, last_latency=%u us, jitter_max=%u us\n",
                   st->tick, st->last_latency_us, st->jitter_us_max);

    uint32_t jbuf[16];
    uint32_t jn = rt_timer_jitter_copy(jbuf, 16);
    if (jn > 0) {
        uint32_t jmax = 0;
        for (uint32_t i = 0; i < jn; i++) {
            if (jbuf[i] > jmax) jmax = jbuf[i];
        }
        console_printf("M0DEMO: rt_timer jitter samples=%u (last %u) max=%u us\n",
                       rt_timer_jitter_samples(), jn, jmax);
    }

    uint32_t ttick[ACT_TRACE_MAX];
    float tlevel[ACT_TRACE_MAX];
    uint32_t tn = emg_act_trace_copy(ttick, tlevel, ACT_TRACE_MAX);
    console_printf("M0DEMO: actuator trace (%u changes):\n", tn);
    for (uint32_t i = 0; i < tn; i++) {
        console_printf("M0DEMO:   tick=%u level=%f\n",
                       ttick[i], (double)tlevel[i]);
    }
    console_printf("M0DEMO: estop asserted at end: %s\n",
                   st->estop ? "yes" : "NO — FAIL");
    return st->estop ? 0 : -1;
}

/* ============================================================================
 * Dispatch (single hunk in core/stubs.c, like cmd_motor)
 * ============================================================================ */

static void cmd_emg_usage(void)
{
    console_printf("emg                    EMG pipeline state dump\n");
    console_printf("emgcal                 calibrate (2s rest baseline, 2s MVC)\n");
    console_printf("emgtest synthetic      scripted synthetic run + transition log\n");
    console_printf("m0demo                 10s EMG->actuator bench loop, estop at end\n");
}

/* Returns 1 when the command was handled. */
int cmd_emg_dispatch(const char *command)
{
    if (!command) {
        return 0;
    }

    if (strcmp(command, "emg") == 0) {
        cmd_emg_status();
        return 1;
    }
    if (strcmp(command, "emgcal") == 0) {
        emg_calibrate();
        return 1;
    }
    if (strncmp(command, "emgtest", 7) == 0 &&
        (command[7] == '\0' || command[7] == ' ')) {
        const char *args = command + 7;
        while (*args == ' ') args++;
        if (strcmp(args, "synthetic") == 0) {
            cmd_emgtest_synthetic();
        } else {
            console_printf("emgtest: unknown mode '%s' (try: emgtest synthetic)\n",
                           args);
        }
        return 1;
    }
    if (strcmp(command, "m0demo") == 0) {
        cmd_m0demo();
        return 1;
    }
    if (strncmp(command, "emg", 3) == 0 &&
        (command[3] == '\0' || command[3] == ' ')) {
        cmd_emg_usage();
        return 1;
    }
    return 0;
}
