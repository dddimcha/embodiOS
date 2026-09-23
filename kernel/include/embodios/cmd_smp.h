/* EMBODIOS SMP Shell Commands (cpus/smpwork)
 *
 * Dispatch hook for the kernel shell: process_command() in core/stubs.c
 * forwards unrecognized commands to cmd_smp_dispatch().
 */

#ifndef EMBODIOS_CMD_SMP_H
#define EMBODIOS_CMD_SMP_H

/**
 * Try to handle a shell command as an SMP command.
 *
 * @return 1 if the command was handled, 0 to fall through to the
 *         shell's "unknown command" path.
 */
int cmd_smp_dispatch(const char* command);

/**
 * Try to handle a shell command as a parallel-matmul benchmark command
 * (`parbench`, core/cmd_parbench.c, v0.7.0 "Maxwell" WS-B). Declared here
 * so the shell dispatcher in core/stubs.c needs a single registration line.
 *
 * @return 1 if the command was handled, 0 to fall through.
 */
int cmd_parbench_dispatch(const char* command);

#endif /* EMBODIOS_CMD_SMP_H */
