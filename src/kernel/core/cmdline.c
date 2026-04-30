/* Kernel cmdline parser. Reads opt/cmdline from QEMU fw_cfg, splits on
 * whitespace, dispatches per token. Linux-compatible subset for now. */

#include "core/cmdline.h"
#include "hw/fw_cfg.h"

#define CMDLINE_MAX 1024

static char g_cmdline[CMDLINE_MAX];
static int  g_inited;
static int  g_console_enabled;
static uint16_t g_console_port;
static int  g_console_baud = 115200;

/* Parse a non-negative decimal integer from `s`, stop at first non-digit.
 * Returns parsed value; *end is set to char after last digit. */
static int parse_dec(const char *s, const char **end) {
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    if (end) *end = s;
    return v;
}

/* `tok` points into g_cmdline at the start of one whitespace-separated
 * token. The token is NUL-terminated by the splitter in cmdline_init. */
static void parse_token(const char *tok) {
    /* console=ttyS<N>[,<baud>][,<rest>] */
    if (tok[0]=='c' && tok[1]=='o' && tok[2]=='n' && tok[3]=='s' &&
        tok[4]=='o' && tok[5]=='l' && tok[6]=='e' && tok[7]=='=') {
        const char *v = tok + 8;
        if (v[0]=='t' && v[1]=='t' && v[2]=='y' && v[3]=='S' &&
            v[4] >= '0' && v[4] <= '9') {
            int n = v[4] - '0';
            if (n == 0)      g_console_port = 0x3F8;
            else if (n == 1) g_console_port = 0x2F8;
            else return;     /* ttyS2+ not wired yet */
            g_console_enabled = 1;
            const char *p = v + 5;
            if (*p == ',') {
                int b = parse_dec(p + 1, 0);
                if (b > 0) g_console_baud = b;
            }
        }
        return;
    }
    /* future: earlycon=, loglevel=, quiet, debug, panic= ... */
}

void cmdline_init(void) {
    if (g_inited) return;
    g_inited = 1;
    int n = fw_cfg_read_file("opt/cmdline", g_cmdline, CMDLINE_MAX);
    if (n <= 0) { g_cmdline[0] = 0; return; }

    /* Split on whitespace, NUL-terminate each token in-place. */
    char *p = g_cmdline;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        char *tok = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        char saved = *p;
        if (saved) *p = 0;
        parse_token(tok);
        if (saved) *p++ = saved;
    }
}

int      cmdline_console_enabled(void) { return g_console_enabled; }
uint16_t cmdline_console_port(void)    { return g_console_port; }
int      cmdline_console_baud(void)    { return g_console_baud; }
const char *cmdline_raw(void)          { return g_cmdline; }
