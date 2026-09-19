/* EMBODIOS serial-UX module: banner, colors, boxes, progress bar.
 * Built on console_printf/console_putchar; no libc. */

#include <embodios/ui.h>
#include <embodios/console.h>
#include <embodios/gguf_parser.h>

int ui_color_enabled = 1;

void ui_set_color(int enabled)
{
    ui_color_enabled = enabled ? 1 : 0;
}

const char* ui_c(const char* code)
{
    return ui_color_enabled ? code : "";
}

/* --- low-level helpers -------------------------------------------------- */

static void ui_puts(const char* s)
{
    console_printf("%s", s);
}

static void ui_repeat(const char* s, int n)
{
    for (int i = 0; i < n; i++)
        ui_puts(s);
}

static int ui_strlen(const char* s)
{
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

/* --- banner ------------------------------------------------------------- */

void ui_banner(void)
{
    extern const char* kernel_version;

    ui_puts("\n");
    ui_puts(ui_c(UI_CYAN)); ui_puts(ui_c(UI_BOLD));
    ui_puts("  ███████╗███╗   ███╗██████╗  ██████╗ ██████╗ ██╗ ██████╗ ███████╗\n");
    ui_puts("  ██╔════╝████╗ ████║██╔══██╗██╔═══██╗██╔══██╗██║██╔═══██╗██╔════╝\n");
    ui_puts("  █████╗  ██╔████╔██║██████╔╝██║   ██║██║  ██║██║██║   ██║███████╗\n");
    ui_puts("  ██╔══╝  ██║╚██╔╝██║██╔══██╗██║   ██║██║  ██║██║██║   ██║╚════██║\n");
    ui_puts("  ███████╗██║ ╚═╝ ██║██████╔╝╚██████╔╝██████╔╝██║╚██████╔╝███████║\n");
    ui_puts("  ╚══════╝╚═╝     ╚═╝╚═════╝  ╚═════╝ ╚═════╝ ╚═╝ ╚═════╝ ╚══════╝\n");
    ui_puts(ui_c(UI_RESET));
    console_printf("  %sEMBODIOS %s%s %s· codename Figaro%s\n",
                   ui_c(UI_BOLD), kernel_version, ui_c(UI_RESET),
                   ui_c(UI_DIM), ui_c(UI_RESET));
    console_printf("  %sOne binary. Any machine. No OS.%s\n\n",
                   ui_c(UI_DIM), ui_c(UI_RESET));
}

/* --- boxes -------------------------------------------------------------- */

#define UI_BOX_WIDTH 56

void ui_hline(int width)
{
    ui_repeat("─", width);
}

void ui_box_top(const char* title)
{
    ui_puts(ui_c(UI_DIM));
    ui_puts(" ┌─ ");
    ui_puts(ui_c(UI_RESET));
    ui_puts(ui_c(UI_BOLD));
    ui_puts(title ? title : "");
    ui_puts(ui_c(UI_RESET));
    ui_puts(ui_c(UI_DIM));
    ui_puts(" ");
    ui_hline(UI_BOX_WIDTH - ui_strlen(title) - 6);
    ui_puts("┐\n");
    ui_puts(ui_c(UI_RESET));
}

void ui_box_bottom(void)
{
    ui_puts(ui_c(UI_DIM));
    ui_puts(" └");
    ui_hline(UI_BOX_WIDTH - 3);
    ui_puts("┘\n");
    ui_puts(ui_c(UI_RESET));
}

/* --- progress bar ------------------------------------------------------- */

#define UI_BAR_WIDTH 16

void ui_progress(const char* label, uint32_t cur, uint32_t total)
{
    static int last_pct = -1;

    if (total == 0) total = 1;
    if (cur > total) cur = total;

    uint32_t pct = (cur * 100) / total;
    if ((int)pct == last_pct && cur < total)
        return;  /* throttle: redraw only when the percent changes */

    int fill = (int)((cur * UI_BAR_WIDTH) / total);

    console_printf("\r %s", ui_c(UI_CYAN));
    /* Label, padded to 20 chars for a stable bar position */
    {
        int len = ui_strlen(label);
        console_printf("%s", label);
        for (int i = len; i < 20; i++) console_putchar(' ');
    }
    console_printf("%s[%s", ui_c(UI_RESET), ui_c(UI_GREEN));
    ui_repeat("█", fill);
    ui_puts(ui_c(UI_DIM));
    ui_repeat("░", UI_BAR_WIDTH - fill);
    console_printf("%s]%s %3u%% (%u/%u)%s",
                   ui_c(UI_DIM), ui_c(UI_RESET),
                   pct, cur, total,
                   cur >= total ? "\n" : "");
    console_flush();

    last_pct = (cur >= total) ? -1 : (int)pct;
}

/* --- leveled messages --------------------------------------------------- */

static void ui_vmsg(const char* color, const char* tag,
                    const char* fmt, __builtin_va_list args)
{
    char buf[256];
    int pos = 0;

    /* Minimal %s/%d/%u/%llu formatting (console_printf has no va_list API) */
    for (const char* p = fmt; *p && pos < (int)sizeof(buf) - 24; p++) {
        if (*p != '%') {
            buf[pos++] = *p;
            continue;
        }
        p++;
        if (*p == 's') {
            const char* s = __builtin_va_arg(args, const char*);
            while (s && *s && pos < (int)sizeof(buf) - 24)
                buf[pos++] = *s++;
        } else if (*p == 'd' || *p == 'u' || *p == 'i') {
            int v = __builtin_va_arg(args, int);
            char tmp[16];
            int n = 0, neg = 0;
            unsigned int uv = (unsigned int)v;
            if (*p != 'u' && v < 0) { neg = 1; uv = (unsigned int)(-v); }
            if (uv == 0) tmp[n++] = '0';
            while (uv) { tmp[n++] = (char)('0' + uv % 10); uv /= 10; }
            if (neg) tmp[n++] = '-';
            while (n > 0) buf[pos++] = tmp[--n];
        } else if (*p == 'l' && p[1] == 'l' && (p[2] == 'u' || p[2] == 'd')) {
            unsigned long long v = __builtin_va_arg(args, unsigned long long);
            char tmp[24];
            int n = 0;
            p += 2;
            if (v == 0) tmp[n++] = '0';
            while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
            while (n > 0) buf[pos++] = tmp[--n];
        } else {
            buf[pos++] = '%';
            if (*p) buf[pos++] = *p;
        }
    }
    buf[pos] = '\0';

    console_printf(" %s%s%s%s %s%s\n",
                   color, ui_c(UI_BOLD), tag, ui_c(UI_RESET),
                   buf, ui_c(UI_RESET));
}

void ui_ok(const char* fmt, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    ui_vmsg(ui_c(UI_GREEN), "[OK]", fmt, args);
    __builtin_va_end(args);
}

void ui_info(const char* fmt, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    ui_vmsg(ui_c(UI_CYAN), "[..]", fmt, args);
    __builtin_va_end(args);
}

void ui_warn(const char* fmt, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    ui_vmsg(ui_c(UI_YELLOW), "[!]", fmt, args);
    __builtin_va_end(args);
}

void ui_err(const char* fmt, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    ui_vmsg(ui_c(UI_RED), "[ERR]", fmt, args);
    __builtin_va_end(args);
}

/* --- shell prompt -------------------------------------------------------- */

void ui_prompt(void)
{
    console_printf("%s%sembodios>%s ", ui_c(UI_CYAN), ui_c(UI_BOLD), ui_c(UI_RESET));
}

/* --- model ready / boot info -------------------------------------------- */

/* GGUF general.file_type (llama.cpp ftype) -> quant name */
static const char* ui_ftype_name(uint32_t ft)
{
    switch (ft) {
    case 0:  return "F32";
    case 1:  return "F16";
    case 2:  return "Q4_0";
    case 3:  return "Q4_1";
    case 7:  return "Q8_0";
    case 8:  return "Q5_0";
    case 9:  return "Q5_1";
    case 10: return "Q2_K";
    case 11: return "Q3_K_S";
    case 12: return "Q3_K_M";
    case 13: return "Q3_K_L";
    case 14: return "Q4_K_S";
    case 15: return "Q4_K_M";
    case 16: return "Q5_K_S";
    case 17: return "Q5_K_M";
    case 18: return "Q6_K";
    default: return NULL;
    }
}

void ui_model_ready(uint64_t load_ms)
{
    extern const char* gguf_get_model_name(void);
    extern const struct gguf_model_arch* gguf_parser_get_arch(void);

    const struct gguf_model_arch* arch = gguf_parser_get_arch();
    const char* name = gguf_get_model_name();
    const char* quant = arch ? ui_ftype_name(arch->general_file_type) : NULL;

    console_printf("\n %s✔ Model ready:%s %s%s%s (%s)",
                   ui_c(UI_GREEN), ui_c(UI_RESET),
                   ui_c(UI_BOLD), name ? name : "unknown", ui_c(UI_RESET),
                   quant ? quant : "?");
    if (arch) {
        console_printf(" · %u layers · vocab %u",
                       arch->block_count, arch->vocab_size);
    }
    console_printf("\n %s   loaded in %llu ms%s\n",
                   ui_c(UI_DIM), (unsigned long long)load_ms, ui_c(UI_RESET));
    console_printf(" %s Try:%s chat <message> · demo · help\n\n",
                   ui_c(UI_DIM), ui_c(UI_RESET));
}

void ui_boot_model_info(void)
{
    extern int gguf_model_embedded(void);
    extern const uint8_t* get_embedded_gguf_model(size_t* out_size);

    if (gguf_model_embedded()) {
        size_t sz = 0;
        get_embedded_gguf_model(&sz);
        console_printf(" %s[OK]%s GGUF model embedded (%u MB) — loads on first chat\n",
                       ui_c(UI_GREEN), ui_c(UI_RESET),
                       (unsigned)(sz / (1024 * 1024)));
    } else {
        console_printf(" %s[!]%s No GGUF model embedded.\n",
                       ui_c(UI_YELLOW), ui_c(UI_RESET));
        console_printf("     Rebuild with: make GGUF_MODEL=/path/to/model.gguf\n");
    }
    console_printf(" %s Commands:%s help · demo · chat <message> · status\n\n",
                   ui_c(UI_DIM), ui_c(UI_RESET));
}
