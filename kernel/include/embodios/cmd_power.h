/* EMBODIOS Power Management Shell Commands (shutdown/reboot/power)
 *
 * Dispatch hook for the kernel shell: process_command() in core/stubs.c
 * forwards commands to cmd_power_dispatch().
 */

#ifndef EMBODIOS_CMD_POWER_H
#define EMBODIOS_CMD_POWER_H

/**
 * Try to handle a shell command as a power-management command
 * (shutdown, poweroff, halt, reboot, power).
 *
 * @return 1 if the command was handled, 0 to fall through to the
 *         shell's "unknown command" path.
 */
int cmd_power_dispatch(const char* command);

/**
 * Power the machine off via ACPI S5 (PM1a_CNT: SLP_TYP|SLP_EN).
 * Probes the PCI ACPI function (PIIX4 / ICH9) for PM1a_CNT_BLK,
 * falls back to the QEMU default I/O base 0x600.
 * Does not return on success.
 */
void power_shutdown(void);

/**
 * Hard-reset the machine: PCI reset control (port 0xCF9), then
 * 8042 keyboard-controller pulse, then a deliberate triple fault.
 * Does not return on success.
 */
void power_reboot(void);

#endif /* EMBODIOS_CMD_POWER_H */
