/* EMBODIOS Real-Time Motor Control Demo
 *
 * Closed-loop PID control of a simulated 2nd-order DC motor running on
 * the rt_timer periodic callback layer (tick IRQ context, up to 1 kHz
 * with the LAPIC tick):
 *
 *   - Plant:  J*w' = Kt*u - B*w (speed dynamics), theta' = w. In 'dc'
 *     mode the loop regulates speed (rad/s); in 'servo' mode it
 *     regulates position (rad) — the same 2nd-order plant, different
 *     feedback and gains.
 *   - Controller: discrete PID (float; rt_timer preserves the FPU/SSE
 *     state of the interrupted context around every invocation) with
 *     integral anti-windup and 8-bit duty saturation.
 *   - Actuator: one byte per control tick to IO port 0xE9 (QEMU
 *     isa-debugcon captures it; the write is harmless on real hardware).
 *     A trace snapshot is handed to task context every 1000th tick —
 *     no console output from IRQ context.
 *   - Jitter: rt_timer records per-callback lateness (rdtsc delta vs the
 *     requested period) in microseconds; the report is count / mean /
 *     stddev / min / max / p99 over up to 8192 samples, cyclictest-style.
 *   - LLM hook: the control loop never blocks. When enabled, a flag set
 *     from IRQ context is watched from the (task-context) run loop and
 *     triggers one short generation whose reply sets the setpoint with a
 *     single atomic 32-bit store.
 */

#include <embodios/motor.h>
#include <embodios/rt_timer.h>
#include <embodios/lapic_timer.h>
#include <embodios/console.h>
#include <embodios/tsc.h>
#include <embodios/types.h>
#include <embodios/kernel.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

extern float sqrtf(float x);

/* Actuator port: QEMU isa-debugcon (-device isa-debugcon,iobase=0xe9) */
#define MOTOR_ACTUATOR_PORT 0xE9

/* Plant constants (2nd-order DC motor, normalized units) */
#define MOTOR_J   0.01f    /* inertia */
#define MOTOR_B   0.05f    /* viscous friction */
#define MOTOR_KT  5.0f     /* torque constant per unit duty; w_ss = Kt/B = 100 rad/s full scale */

#define JITTER_MAX_SAMPLES 8192

typedef enum {
    PLANT_DC = 0,          /* speed control, setpoint in rad/s */
    PLANT_SERVO = 1        /* position control, setpoint in rad */
} plant_kind_t;

static struct {
    volatile int  running;
    plant_kind_t  plant;
    float         kp, ki, kd;
    volatile float setpoint;        /* rad/s (dc) or rad (servo) */

    /* Plant + controller state (owned by the IRQ callback while running) */
    float         theta;            /* position, rad */
    float         omega;            /* speed, rad/s */
    float         integ;            /* PID integral */
    float         prev_err;         /* previous error (derivative) */
    uint32_t      hz;               /* loop rate */
    uint64_t      ticks;            /* control ticks this run */

    /* Trace handoff IRQ -> task context (single pending snapshot) */
    volatile int  trace_pending;
    uint64_t      trace_tick;
    float         trace_pv;
    float         trace_duty;
    float         trace_sp;

    /* LLM policy hook */
    int           llm_enabled;
    volatile int  llm_request;
} motor = {
    .running = 0,
    .plant = PLANT_DC,
    .kp = 6.0f, .ki = 10.0f, .kd = 0.02f,
    .setpoint = 50.0f,
};

static uint32_t jitter_buf[JITTER_MAX_SAMPLES];

/* ============================================================================
 * Low-level helpers
 * ============================================================================ */

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" :: "a"(value), "Nd"(port));
}

/* Wall-clock milliseconds (rdtsc-based; hal fallback if TSC uncalibrated) */
static uint64_t motor_now_ms(void)
{
    uint64_t freq = tsc_get_frequency();
    if (freq > 0) {
        return (rdtsc() * 1000ULL) / freq;
    }
    extern uint64_t hal_timer_get_milliseconds(void);
    return hal_timer_get_milliseconds();
}

/* ============================================================================
 * Control loop (tick IRQ context via rt_timer)
 * ============================================================================ */

static void motor_control_tick(uint64_t tick, void *arg)
{
    (void)tick;
    (void)arg;

    float dt = 1.0f / (float)motor.hz;

    float sp = motor.setpoint;
    float pv = (motor.plant == PLANT_SERVO) ? motor.theta : motor.omega;
    float err = sp - pv;

    /* PID with integral anti-windup and 8-bit duty saturation */
    motor.integ += err * dt;
    if (motor.integ > 25.0f)  motor.integ = 25.0f;
    if (motor.integ < -25.0f) motor.integ = -25.0f;
    float deriv = (err - motor.prev_err) / dt;
    motor.prev_err = err;

    float u = motor.kp * err + motor.ki * motor.integ + motor.kd * deriv;
    if (u < 0.0f)   u = 0.0f;
    if (u > 255.0f) u = 255.0f;

    /* 2nd-order DC motor plant, forward Euler */
    float un  = u * (1.0f / 255.0f);
    float acc = (MOTOR_KT * un - MOTOR_B * motor.omega) * (1.0f / MOTOR_J);
    motor.omega += acc * dt;
    motor.theta += motor.omega * dt;

    /* Actuator: one byte per control tick */
    outb(MOTOR_ACTUATOR_PORT, (uint8_t)u);

    motor.ticks++;
    if ((motor.ticks % 1000) == 0 && !motor.trace_pending) {
        motor.trace_tick  = motor.ticks;
        motor.trace_pv    = pv;
        motor.trace_duty  = u;
        motor.trace_sp    = sp;
        motor.trace_pending = 1;
    }

    if (motor.llm_enabled) {
        motor.llm_request = 1;      /* watched from task context */
    }
}

/* ============================================================================
 * LLM policy hook (task context only)
 * ============================================================================ */

static int parse_first_float(const char *s, float *out)
{
    if (!s || !out) {
        return -1;
    }
    while (*s) {
        if ((*s >= '0' && *s <= '9') || *s == '.' || *s == '-') {
            char *end = NULL;
            double v = strtod(s, &end);
            if (end != s) {
                *out = (float)v;
                return 0;
            }
        }
        s++;
    }
    return -1;
}

static void motor_llm_policy(void)
{
    char prompt[256];
    char reply[256];

    float pv = (motor.plant == PLANT_SERVO) ? motor.theta : motor.omega;
    const char *unit = (motor.plant == PLANT_SERVO) ? "rad" : "rad/s";
    float max_sp = (motor.plant == PLANT_SERVO) ? 6.28f : 100.0f;

    /* Build the prompt by hand (no snprintf in the kernel console) */
    char *p = prompt;
    const char *q = "Motor control policy. Current value ";
    while (*q) *p++ = *q++;
    int ip = (int)pv;               /* integer part is enough for a policy */
    if (ip < 0) { *p++ = '-'; ip = -ip; }
    char num[16];
    int ni = 0;
    do { num[ni++] = (char)('0' + (ip % 10)); ip /= 10; } while (ip && ni < 15);
    while (ni > 0) *p++ = num[--ni];
    *p++ = ' ';
    while (*unit) *p++ = *unit++;
    q = ". Reply with a single number: the new setpoint (0..100).";
    while (*q) *p++ = *q++;
    *p = '\0';

    console_printf("MOTOR: LLM policy query (task context, loop keeps running)...\n");
    if (motor_llm_query(prompt, reply, sizeof(reply)) != 0) {
        console_printf("MOTOR: LLM policy unavailable (no model reply)\n");
        return;
    }

    float new_sp;
    if (parse_first_float(reply, &new_sp) != 0) {
        console_printf("MOTOR: LLM reply had no number, setpoint unchanged\n");
        return;
    }
    if (new_sp < 0.0f)     new_sp = 0.0f;
    if (new_sp > max_sp)   new_sp = max_sp;

    motor.setpoint = new_sp;        /* single 32-bit store: irq-safe */
    console_printf("MOTOR: LLM policy setpoint -> %f %s\n",
                   (double)new_sp, (motor.plant == PLANT_SERVO) ? "rad" : "rad/s");
}

/* ============================================================================
 * Jitter report
 * ============================================================================ */

static void sort_u32(uint32_t *a, uint32_t n)
{
    /* Simple quicksort (median-of-3 pivot), n <= 8192 */
    if (n < 2) {
        return;
    }
    uint32_t pivot = a[n / 2];
    uint32_t i = 0, j = n - 1;
    for (;;) {
        while (a[i] < pivot) i++;
        while (a[j] > pivot) j--;
        if (i >= j) break;
        uint32_t t = a[i]; a[i] = a[j]; a[j] = t;
        i++; if (j > 0) j--;
    }
    if (j + 1 < n) sort_u32(a, j + 1);
    if (i < n)     sort_u32(a + i, n - i);
}

static void motor_print_jitter(void)
{
    uint32_t n = rt_timer_jitter_copy(jitter_buf, JITTER_MAX_SAMPLES);
    if (n == 0) {
        console_printf("JITTER: no samples (run 'motor run <ms> [hz]' first)\n");
        return;
    }

    double sum = 0.0, sumsq = 0.0;
    uint32_t vmin = 0xFFFFFFFFU, vmax = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = jitter_buf[i];
        sum += (double)v;
        sumsq += (double)v * (double)v;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }

    sort_u32(jitter_buf, n);

    double mean = sum / (double)n;
    double var = sumsq / (double)n - mean * mean;
    if (var < 0.0) var = 0.0;
    double stddev = (double)sqrtf((float)var);
    uint32_t p99 = jitter_buf[((uint64_t)(n - 1) * 99ULL) / 100ULL];

    console_printf("JITTER: count=%u mean=%f us stddev=%f us min=%u us max=%u us p99=%u us\n",
                   n, mean, stddev, vmin, vmax, p99);
}

/* ============================================================================
 * Run command
 * ============================================================================ */

static void motor_run(uint64_t ms, uint32_t hz)
{
    if (motor.running) {
        console_printf("MOTOR: already running\n");
        return;
    }

    uint32_t tick_hz = rt_timer_get_tick_hz();
    uint32_t requested = hz;
    if (hz == 0) { hz = 1000; requested = 1000; }
    if (hz > 1000) hz = 1000;
    if (hz > tick_hz) hz = tick_hz;   /* cannot outrun the tick source */
    if (ms == 0) ms = 5000;

    /* Reset controller + plant state */
    motor.theta = 0.0f;
    motor.omega = 0.0f;
    motor.integ = 0.0f;
    motor.prev_err = 0.0f;
    motor.ticks = 0;
    motor.trace_pending = 0;
    motor.llm_request = 0;
    motor.hz = hz;

    console_printf("MOTOR: run %llu ms @ %u Hz, plant=%s setpoint=%f %s, pid=(%f,%f,%f)\n",
                   (unsigned long long)ms, hz,
                   motor.plant == PLANT_SERVO ? "servo" : "dc",
                   (double)motor.setpoint,
                   motor.plant == PLANT_SERVO ? "rad" : "rad/s",
                   (double)motor.kp, (double)motor.ki, (double)motor.kd);
    if (hz != requested) {
        console_printf("MOTOR: note — requested %u Hz clamped to %u Hz (%s tick @ %u Hz)\n",
                       requested, hz,
                       lapic_timer_active() ? "LAPIC" : "PIT fallback", tick_hz);
    }

    rt_timer_jitter_arm();
    if (rt_timer_register(motor_control_tick, 1000000U / hz, NULL) != 0) {
        rt_timer_jitter_disarm();
        console_printf("MOTOR: rt_timer_register failed\n");
        return;
    }
    motor.running = 1;

    uint64_t start_ms = motor_now_ms();
    uint64_t last_llm_ms = 0;
    while ((motor_now_ms() - start_ms) < ms) {
        /* Trace handoff: print the snapshot recorded by the 1000th tick */
        if (motor.trace_pending) {
            console_printf("MOTOR: tick=%llu sp=%f pv=%f duty=%f\n",
                           (unsigned long long)motor.trace_tick,
                           (double)motor.trace_sp,
                           (double)motor.trace_pv,
                           (double)motor.trace_duty);
            motor.trace_pending = 0;
        }

        /* LLM policy hook: flag from IRQ context, generation here in
         * task context (the control loop keeps running via the tick). */
        if (motor.llm_enabled && motor.llm_request &&
            (motor_now_ms() - start_ms) - last_llm_ms >= 2000) {
            motor.llm_request = 0;
            last_llm_ms = motor_now_ms() - start_ms;
            motor_llm_policy();
        }

        __asm__ volatile("pause");
    }

    rt_timer_unregister(motor_control_tick);
    rt_timer_jitter_disarm();
    outb(MOTOR_ACTUATOR_PORT, 0);       /* actuator to safe state */
    motor.running = 0;

    console_printf("MOTOR: run complete — %llu control ticks @ %u Hz\n",
                   (unsigned long long)motor.ticks, motor.hz);
    motor_print_jitter();
}

/* ============================================================================
 * Command dispatch
 * ============================================================================ */

static void motor_usage(void)
{
    console_printf("motor run <ms> [hz]    run closed loop (default 5000 ms @ 1000 Hz)\n");
    console_printf("motor jitter           re-print last jitter report\n");
    console_printf("motor llm on|off       LLM policy adjusts setpoint (default off)\n");
    console_printf("motor plant dc|servo   plant model (default dc)\n");
    console_printf("motor pid <kp> <ki> <kd>  set PID gains (no args: print)\n");
}

void cmd_motor_dispatch(const char *args)
{
    /* Tokenize (bounded local copy) */
    char buf[128];
    unsigned int bi = 0;
    while (args && *args && bi < sizeof(buf) - 1) {
        buf[bi++] = *args++;
    }
    buf[bi] = '\0';

    char *argv[8];
    int argc = 0;
    char *s = buf;
    while (*s && argc < 8) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        argv[argc++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (*s) *s++ = '\0';
    }

    if (argc == 0) {
        motor_usage();
        return;
    }

    if (strcmp(argv[0], "run") == 0) {
        uint64_t ms = (argc > 1) ? (uint64_t)strtoull(argv[1], NULL, 10) : 5000;
        uint32_t hz = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 10) : 1000;
        motor_run(ms, hz);
    } else if (strcmp(argv[0], "jitter") == 0) {
        motor_print_jitter();
    } else if (strcmp(argv[0], "llm") == 0) {
        if (argc > 1 && strcmp(argv[1], "on") == 0) {
            motor.llm_enabled = 1;
            console_printf("MOTOR: LLM policy hook ON (setpoint adjusts during 'motor run')\n");
        } else if (argc > 1 && strcmp(argv[1], "off") == 0) {
            motor.llm_enabled = 0;
            console_printf("MOTOR: LLM policy hook OFF\n");
        } else {
            console_printf("MOTOR: llm is %s\n", motor.llm_enabled ? "on" : "off");
        }
    } else if (strcmp(argv[0], "plant") == 0) {
        if (argc > 1 && strcmp(argv[1], "servo") == 0) {
            motor.plant = PLANT_SERVO;
            motor.kp = 80.0f; motor.ki = 5.0f; motor.kd = 12.0f;
            motor.setpoint = 3.14f;
            console_printf("MOTOR: plant=servo (position control), setpoint=3.14 rad, pid=(80,5,12)\n");
        } else if (argc > 1 && strcmp(argv[1], "dc") == 0) {
            motor.plant = PLANT_DC;
            motor.kp = 6.0f; motor.ki = 10.0f; motor.kd = 0.02f;
            motor.setpoint = 50.0f;
            console_printf("MOTOR: plant=dc (speed control), setpoint=50 rad/s, pid=(6,10,0.02)\n");
        } else {
            console_printf("MOTOR: plant is %s\n",
                           motor.plant == PLANT_SERVO ? "servo" : "dc");
        }
    } else if (strcmp(argv[0], "pid") == 0) {
        if (argc >= 4) {
            motor.kp = (float)strtod(argv[1], NULL);
            motor.ki = (float)strtod(argv[2], NULL);
            motor.kd = (float)strtod(argv[3], NULL);
            console_printf("MOTOR: pid=(%f,%f,%f)\n",
                           (double)motor.kp, (double)motor.ki, (double)motor.kd);
        } else {
            console_printf("MOTOR: pid=(%f,%f,%f)\n",
                           (double)motor.kp, (double)motor.ki, (double)motor.kd);
        }
    } else {
        motor_usage();
    }
}
