/* EMBODIOS Storage Shell Commands (fls/fsave/fload/frm/df/fstest)
 *
 * Dispatch hook for the kernel shell: process_command() in core/stubs.c
 * forwards unrecognized commands to cmd_storage_dispatch().
 */

#ifndef EMBODIOS_CMD_STORAGE_H
#define EMBODIOS_CMD_STORAGE_H

/**
 * Try to handle a shell command as a storage command.
 *
 * @return 1 if the command was handled, 0 to fall through to the
 *         shell's "unknown command" path.
 */
int cmd_storage_dispatch(const char* command);

/**
 * Persist sampling/chat settings to the embfs "config" file.
 * Silent no-op when no disk/volume is available.
 */
void cmd_storage_config_autosave(void);

/**
 * Load the embfs "config" file and apply settings (boot time).
 * Silent when no disk/volume/config exists.
 *
 * @return 0 if a config was applied, negative otherwise.
 */
int cmd_storage_config_load(void);

#endif /* EMBODIOS_CMD_STORAGE_H */
