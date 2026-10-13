#ifndef EMBODIOS_ACTUATOR_H
#define EMBODIOS_ACTUATOR_H
/* Actuator contract — SPEC-M0. DO NOT EDIT (integration contract). */

int         actuator_init(void);            /* weak stub; WS-DRV overrides */
int         actuator_set(float level);      /* 0..1 -> goal, slew-limited */
int         actuator_estop(void);           /* torque OFF immediately */
const char *actuator_name(void);            /* "stub" / "feetech-sts3215" */

#endif
