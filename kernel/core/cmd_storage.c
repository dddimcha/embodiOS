/* EMBODIOS Storage Shell Commands
 *
 * Shell commands for the embfs mini filesystem and persistent config:
 *   fls                    List files
 *   fsave <name> <text>    Save text to a file
 *   fload <name>           Print file contents
 *   frm <name>             Delete a file
 *   df                     Show usage (free/used)
 *   fformat [force]        Create a fresh embfs volume
 *   fstest                 embfs self-test (SKIP without disk)
 *
 * Config persistence: temp/topp/chatformat changes are autosaved to the
 * embfs file "config" (via cmd_storage_config_autosave, called from the
 * respective setters in stubs.c). At boot a constructor loads it back.
 * Everything is a silent no-op when no disk is attached.
 *
 * Author: EMBODIOS Team
 * License: MIT
 */

#include <embodios/cmd_storage.h>
#include <embodios/embfs.h>
#include <embodios/console.h>
#include <embodios/kernel.h>
#include <embodios/mm.h>
#include <embodios/chat_template.h>
#include <embodios/streaming_inference.h>

#define CONFIG_FILE_NAME  "config"

/* ============================================================================
 * Persistent Config
 * ============================================================================ */

static int float_to_milli(float v)
{
    int m = (int)(v * 1000.0f);
    float frac = v * 1000.0f - (float)m;
    if (frac >= 0.5f) m++;
    return m;
}

/* Append helpers for building "key=value" lines */
static void cfg_puts(char* buf, size_t cap, size_t* n, const char* s)
{
    while (*s && *n < cap - 1) {
        buf[(*n)++] = *s++;
    }
    buf[*n] = '\0';
}

static void cfg_put_int(char* buf, size_t cap, size_t* n, int v)
{
    char tmp[12];
    int i = 0;
    if (v < 0) { cfg_puts(buf, cap, n, "-"); v = -v; }
    if (v == 0) { cfg_puts(buf, cap, n, "0"); return; }
    while (v > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0 && *n < cap - 1) {
        buf[(*n)++] = tmp[--i];
    }
    buf[*n] = '\0';
}

/* Format: three "key=value" lines (text), e.g.
 *   temp=700
 *   topp=900
 *   chatformat=auto
 */
static void config_serialize(char* buf, size_t cap)
{
    int temp_m = float_to_milli(streaming_inference_get_temperature());
    int topp_m = float_to_milli(streaming_inference_get_top_p());
    const char* fmt = chat_template_format_name(chat_template_get_config());

    size_t n = 0;
    buf[0] = '\0';
    cfg_puts(buf, cap, &n, "temp=");
    cfg_put_int(buf, cap, &n, temp_m);
    cfg_puts(buf, cap, &n, "\ntopp=");
    cfg_put_int(buf, cap, &n, topp_m);
    cfg_puts(buf, cap, &n, "\nchatformat=");
    cfg_puts(buf, cap, &n, fmt);
    cfg_puts(buf, cap, &n, "\n");
}

void cmd_storage_config_autosave(void)
{
    /* Silent: only act when an embfs volume is available. Never auto-format
     * here — a bare config change should not partition a blank disk. */
    if (!embfs_mounted() && embfs_mount() != EMBFS_OK) {
        return;
    }

    char buf[128];
    config_serialize(buf, sizeof(buf));

    if (embfs_write(CONFIG_FILE_NAME, buf, strlen(buf)) == EMBFS_OK) {
        console_printf(" [config saved]\n");
    }
}

static int parse_milli(const char* s)
{
    int v = 0;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return neg ? -v : v;
}

int cmd_storage_config_load(void)
{
    if (!embfs_mounted() && embfs_mount() != EMBFS_OK) {
        return EMBFS_ERR_NO_DISK;
    }

    uint32_t size = 0;
    int ret = embfs_read(CONFIG_FILE_NAME, NULL, 0, &size);
    if (ret != EMBFS_OK || size == 0 || size > 4096) {
        return (ret == EMBFS_OK) ? EMBFS_ERR_INVALID : ret;
    }

    char* buf = (char*)heap_alloc(size + 1);
    if (!buf) return EMBFS_ERR_NOMEM;

    ret = embfs_read(CONFIG_FILE_NAME, buf, size, NULL);
    if (ret != EMBFS_OK) {
        heap_free(buf);
        return ret;
    }
    buf[size] = '\0';

    /* Parse key=value lines */
    int applied = 0;
    char* p = buf;
    while (*p) {
        char* eol = strchr(p, '\n');
        if (eol) *eol = '\0';

        if (strncmp(p, "temp=", 5) == 0) {
            streaming_inference_set_temperature(parse_milli(p + 5) / 1000.0f);
            applied++;
        } else if (strncmp(p, "topp=", 5) == 0) {
            streaming_inference_set_top_p(parse_milli(p + 5) / 1000.0f);
            applied++;
        } else if (strncmp(p, "chatformat=", 11) == 0) {
            int fmt = chat_template_parse_name(p + 11);
            if (fmt >= 0) {
                chat_template_set_format((chat_format_t)fmt);
                applied++;
            }
        }

        if (!eol) break;
        p = eol + 1;
    }

    heap_free(buf);

    if (applied > 0) {
        console_printf("[embfs] Config restored: temp=%d.%02d topp=%d.%02d chatformat=%s\n",
                       (int)streaming_inference_get_temperature(),
                       (int)(streaming_inference_get_temperature() * 100) % 100,
                       (int)streaming_inference_get_top_p(),
                       (int)(streaming_inference_get_top_p() * 100) % 100,
                       chat_template_format_name(chat_template_get_config()));
        return 0;
    }
    return EMBFS_ERR_INVALID;
}

/* Boot-time autoload: runs from __init_array after virtio_blk_init.
 * Completely silent when no embfs volume/config exists. */
__attribute__((constructor))
static void cmd_storage_boot_init(void)
{
    cmd_storage_config_load();
}

/* ============================================================================
 * Shell Commands
 * ============================================================================ */

/* Extract the next whitespace-delimited token; advances *args past it. */
static void next_token(const char** args, char* out, size_t cap)
{
    const char* p = *args;
    while (*p == ' ') p++;
    size_t n = 0;
    while (*p && *p != ' ' && n < cap - 1) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    while (*p && *p != ' ') p++;  /* skip rest of over-long token */
    *args = p;
}

static void cmd_fsave(const char* args)
{
    char name[EMBFS_NAME_LEN];
    next_token(&args, name, sizeof(name));

    const char* text = args;
    while (*text == ' ') text++;

    if (name[0] == '\0' || *text == '\0') {
        console_printf("Usage: fsave <name> <text>\n");
        return;
    }

    int ret = embfs_write(name, text, strlen(text));
    if (ret == EMBFS_OK) {
        console_printf("Saved '%s' (%zu bytes)\n", name, strlen(text));
    } else {
        console_printf("fsave: failed (%d)\n", ret);
    }
}

static void cmd_fload(const char* args)
{
    char name[EMBFS_NAME_LEN];
    next_token(&args, name, sizeof(name));

    if (name[0] == '\0') {
        console_printf("Usage: fload <name>\n");
        return;
    }

    uint32_t size = 0;
    int ret = embfs_read(name, NULL, 0, &size);
    if (ret != EMBFS_OK) {
        console_printf("fload: %s (%d)\n",
                       ret == EMBFS_ERR_NOT_FOUND ? "not found" : "error", ret);
        return;
    }

    char* buf = (char*)heap_alloc(size + 1);
    if (!buf) {
        console_printf("fload: out of memory\n");
        return;
    }

    ret = embfs_read(name, buf, size, NULL);
    if (ret != EMBFS_OK) {
        console_printf("fload: read failed (%d)%s\n", ret,
                       ret == EMBFS_ERR_CRC ? " [CRC mismatch]" : "");
        heap_free(buf);
        return;
    }

    buf[size] = '\0';
    console_printf("%s (%u bytes):\n%s\n", name, size, buf);
    heap_free(buf);
}

static void cmd_frm(const char* args)
{
    char name[EMBFS_NAME_LEN];
    next_token(&args, name, sizeof(name));

    if (name[0] == '\0') {
        console_printf("Usage: frm <name>\n");
        return;
    }

    int ret = embfs_delete(name);
    if (ret == EMBFS_OK) {
        console_printf("Deleted '%s'\n", name);
    } else {
        console_printf("frm: %s (%d)\n",
                       ret == EMBFS_ERR_NOT_FOUND ? "not found" : "error", ret);
    }
}

static void cmd_fformat(const char* args)
{
    bool force = false;
    char tok[16];
    next_token(&args, tok, sizeof(tok));
    if (strcmp(tok, "force") == 0) force = true;

    int ret = embfs_format(force);
    if (ret == EMBFS_OK) {
        embfs_mount();
    } else {
        console_printf("fformat: failed (%d)%s\n", ret,
                       force ? "" : " (use 'fformat force' to override)");
    }
}

/* ============================================================================
 * fstest - embfs self-test
 * ============================================================================ */

static int fstest_check(const char* name, const char* expect)
{
    uint32_t size = 0;
    if (embfs_read(name, NULL, 0, &size) != EMBFS_OK) return -1;
    if (size != strlen(expect)) return -2;

    char* buf = (char*)heap_alloc(size + 1);
    if (!buf) return -3;
    int ret = embfs_read(name, buf, size, NULL);
    if (ret != EMBFS_OK) { heap_free(buf); return -4; }
    buf[size] = '\0';
    bool ok = (strcmp(buf, expect) == 0);
    heap_free(buf);
    return ok ? 0 : -5;
}

static void cmd_fstest(void)
{
    console_printf("\n=== embfs self-test (fstest) ===\n");

    int ret = embfs_ensure();
    if (ret != EMBFS_OK) {
        console_printf("SKIP: no embfs-capable disk "
                       "(-drive file=disk.img,format=raw,if=virtio)\n\n");
        return;
    }

    int failures = 0;
    const char* t1 = "Hello, embfs!";
    const char* t2 = "embfs persistence test: the quick brown fox jumps over "
                     "the lazy dog. 0123456789";
    const char* t3 = "config-like\nkey=value\ntemp=700\n";

#define CHECK(label, expr)                                              \
    do {                                                                \
        bool _ok = (expr);                                              \
        console_printf("  %-38s %s\n", label, _ok ? "PASS" : "FAIL");   \
        if (!_ok) failures++;                                           \
    } while (0)

    /* 1. Create + readback + CRC verify (CRC checked inside embfs_read) */
    CHECK("write file1", embfs_write("ftest1", t1, strlen(t1)) == EMBFS_OK);
    CHECK("read file1 (data+CRC)", fstest_check("ftest1", t1) == 0);

    /* 2. Overwrite with different length */
    CHECK("overwrite file1", embfs_write("ftest1", t2, strlen(t2)) == EMBFS_OK);
    CHECK("read overwritten file1", fstest_check("ftest1", t2) == 0);

    /* 3. Multiple files coexist */
    CHECK("write file2", embfs_write("ftest2", t2, strlen(t2)) == EMBFS_OK);
    CHECK("write file3", embfs_write("ftest3", t3, strlen(t3)) == EMBFS_OK);
    CHECK("file1 intact", fstest_check("ftest1", t2) == 0);
    CHECK("file2 intact", fstest_check("ftest2", t2) == 0);
    CHECK("file3 intact", fstest_check("ftest3", t3) == 0);

    /* 4. Delete + space reclaim */
    CHECK("delete file2", embfs_delete("ftest2") == EMBFS_OK);
    CHECK("file2 gone", !embfs_exists("ftest2"));
    CHECK("file1 still intact", fstest_check("ftest1", t2) == 0);

    /* 5. Remount from disk (simulates reboot within one boot) */
    CHECK("remount", embfs_mount() == EMBFS_OK);
    CHECK("file1 survives remount", fstest_check("ftest1", t2) == 0);
    CHECK("file3 survives remount", fstest_check("ftest3", t3) == 0);

    /* 6. Cleanup */
    embfs_delete("ftest1");
    embfs_delete("ftest3");
    CHECK("cleanup leaves no test files",
          !embfs_exists("ftest1") && !embfs_exists("ftest3"));

    console_printf("\nFSTEST: %s (%d failure%s)\n\n",
                   failures == 0 ? "PASS" : "FAIL",
                   failures, failures == 1 ? "" : "s");
#undef CHECK
}

/* ============================================================================
 * Dispatch
 * ============================================================================ */

int cmd_storage_dispatch(const char* command)
{
    if (strcmp(command, "fls") == 0) {
        embfs_list();
    } else if (strncmp(command, "fsave", 5) == 0 &&
               (command[5] == '\0' || command[5] == ' ')) {
        cmd_fsave(command + 5);
    } else if (strncmp(command, "fload", 5) == 0 &&
               (command[5] == '\0' || command[5] == ' ')) {
        cmd_fload(command + 5);
    } else if (strncmp(command, "frm", 3) == 0 &&
               (command[3] == '\0' || command[3] == ' ')) {
        cmd_frm(command + 3);
    } else if (strcmp(command, "df") == 0) {
        embfs_df();
    } else if (strcmp(command, "fstest") == 0) {
        cmd_fstest();
    } else if (strncmp(command, "fformat", 7) == 0 &&
               (command[7] == '\0' || command[7] == ' ')) {
        cmd_fformat(command + 7);
    } else {
        return 0;  /* Not a storage command */
    }
    return 1;
}
