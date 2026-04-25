/* OOM-Score-Adj + /proc/$pid/oom_score{,_adj,_adj} ktests
 *
 * Deckt Phase 17 ab:
 *   - oom_score_adj clamp [-1000, 1000]
 *   - oom_score_adj_min monotonisch via Lowering
 *   - CAP_SYS_RESOURCE-Gate fuer Lowering past min
 *   - fork inherit
 *   - SUID-exec reset (defensiv: simuliert via Pfad-Abdeckung,
 *     SUID-exec ist im Kernel noch nicht implementiert)
 *   - legacy /proc/$pid/oom_adj scale roundtrip
 *   - /proc/$pid/oom_score sanity (0..2000)
 *   - init (PID 1) Protection (oom_score_adj = -1000 immune)
 */
#include "ktest.h"

#define LINUX_CAPABILITY_VERSION_3  0x20080522
#define CAP_SYS_RESOURCE_BIT        24

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

struct cap_header { uint32_t version; int pid; };
struct cap_data   { uint32_t effective; uint32_t permitted; uint32_t inheritable; };

static long read_text_int(const char *path) {
    long fd = sc3(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0) return fd;
    char buf[32] = {0};
    long r = sc3(SYS_READ, fd, (long)buf, 31);
    sc1(SYS_CLOSE, fd);
    if (r <= 0) return -1;
    int i = 0, neg = 0;
    if (buf[i] == '-') { neg = 1; i++; }
    long v = 0;
    while (i < r && buf[i] >= '0' && buf[i] <= '9') {
        v = v * 10 + (buf[i] - '0'); i++;
    }
    return neg ? -v : v;
}

static long write_text(const char *path, const char *s, int len) {
    long fd = sc3(SYS_OPEN, (long)path, O_WRONLY, 0);
    if (fd < 0) return fd;
    long r = sc3(SYS_WRITE, fd, (long)s, len);
    sc1(SYS_CLOSE, fd);
    return r;
}

static int kstrlen(const char *s) {
    int n = 0; while (s[n]) n++; return n;
}

/* ── adj-clamp [-1000, 1000] ─────────────────────────── */

static void test_oom_score_adj_clamp(void) {
    puts("\n[oom/score_adj_clamp]\n");

    long r = write_text("/proc/self/oom_score_adj", "0\n", 2);
    check("write 0 ok", r == 2);
    check_val("readback 0", read_text_int("/proc/self/oom_score_adj"), 0);

    r = write_text("/proc/self/oom_score_adj", "1000\n", 5);
    check("write +1000 ok", r == 5);
    check_val("readback +1000", read_text_int("/proc/self/oom_score_adj"), 1000);

    /* +1001 -> EINVAL */
    r = write_text("/proc/self/oom_score_adj", "1001\n", 5);
    check_val("write +1001 -> EINVAL", r, -EINVAL);

    /* -1001 -> EINVAL */
    r = write_text("/proc/self/oom_score_adj", "-1001\n", 6);
    check_val("write -1001 -> EINVAL", r, -EINVAL);

    /* Garbage -> EINVAL */
    r = write_text("/proc/self/oom_score_adj", "abc\n", 4);
    check_val("write abc -> EINVAL", r, -EINVAL);

    /* Reset auf 0 fuer den Rest der Suite. */
    write_text("/proc/self/oom_score_adj", "0\n", 2);
}
TEST("oom/score_adj_clamp", test_oom_score_adj_clamp);

/* ── adj inherit on fork ─────────────────────────── */

static void test_oom_score_adj_inherit(void) {
    puts("\n[oom/score_adj_inherit]\n");

    long r = write_text("/proc/self/oom_score_adj", "500\n", 4);
    check("parent write 500", r == 4);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        long v = read_text_int("/proc/self/oom_score_adj");
        sc1(SYS_EXIT_GROUP, v == 500 ? 0 : 1);
        __builtin_unreachable();
    }
    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check_val("child inherits adj=500", WEXITSTATUS(status), 0);

    write_text("/proc/self/oom_score_adj", "0\n", 2);
}
TEST("oom/score_adj_inherit", test_oom_score_adj_inherit);

/* ── adj_min CAP_SYS_RESOURCE gate ──────────────────
 *
 * Wenn ein Prozess seinen oom_score_adj einmal auf -500 senkt, ist
 * oom_score_adj_min = -500. Anschliessend ohne CAP_SYS_RESOURCE auf -700
 * senken muss -EPERM. Mit Cap (Default-Test laeuft als root mit voller
 * Cap-Set) muss es gehen. Hier droppen wir die Cap explizit per capset. */

static void test_oom_score_adj_min_gate(void) {
    puts("\n[oom/score_adj_min_gate]\n");

    /* Reset min: einmal auf 0 setzen — min bleibt aber das bisher Niedrigste.
     * Linux: setzen auf hoeheren Wert hebt min nicht an. Daher in Child. */
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Adj auf -200 -> min wird -200. */
        long w = write_text("/proc/self/oom_score_adj", "-200\n", 5);
        if (w != 5) sc1(SYS_EXIT_GROUP, 10);

        /* Cap droppen: leere effective (capset macht das ohne Subset-Verletzung
         * wenn pP gleich bleibt). Subset-Regel: pE ⊆ pP. */
        struct cap_header hdr = { .version = LINUX_CAPABILITY_VERSION_3, .pid = 0 };
        struct cap_data data[2];
        for (int i = 0; i < (int)sizeof(data); i++) ((char *)&data)[i] = 0;
        long gr = sc2(SYS_CAPGET, (long)&hdr, (long)data);
        if (gr != 0) sc1(SYS_EXIT_GROUP, 11);
        /* Drop nur CAP_SYS_RESOURCE aus effective (nicht aus permitted, sonst
         * stricter subset-check). */
        data[0].effective &= ~(1u << CAP_SYS_RESOURCE_BIT);
        long sr = sc2(SYS_CAPSET, (long)&hdr, (long)data);
        if (sr != 0) sc1(SYS_EXIT_GROUP, 12);

        /* Lowering auf -500 ohne Cap -> EPERM. */
        w = write_text("/proc/self/oom_score_adj", "-500\n", 5);
        if (w != -EPERM) sc1(SYS_EXIT_GROUP, 13);

        /* Lateral (-150, hoeher als -200) muss erlaubt sein. */
        w = write_text("/proc/self/oom_score_adj", "-150\n", 5);
        if (w != 5) sc1(SYS_EXIT_GROUP, 14);

        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }
    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check_val("child cap-gate behaviour", WEXITSTATUS(status), 0);
}
TEST("oom/score_adj_min_gate", test_oom_score_adj_min_gate);

/* ── legacy /proc/$pid/oom_adj scale roundtrip ───── */

static void test_oom_adj_legacy(void) {
    puts("\n[oom/adj_legacy]\n");

    /* Reset zu 0 */
    write_text("/proc/self/oom_score_adj", "0\n", 2);
    check_val("oom_adj reads 0 after reset", read_text_int("/proc/self/oom_adj"), 0);

    /* oom_adj=15 -> oom_score_adj = 15*1000/17 = 882 (truncated).
     * Reverse: 882*17/1000 = 14994/1000 = 14 (Linux mm/oom_kill.c — bekannte
     * Quantisierungsverlust durch Integer-Skalierung in beide Richtungen). */
    long w = write_text("/proc/self/oom_adj", "15\n", 3);
    check("write oom_adj=15", w == 3);
    check_val("oom_score_adj == 882", read_text_int("/proc/self/oom_score_adj"), 882);
    check_val("oom_adj reads 14 back (rounding)", read_text_int("/proc/self/oom_adj"), 14);

    /* oom_adj=-17 (OOM_DISABLE) -> oom_score_adj = -1000 (immune). */
    /* Achtung: -17 < min muss CAP_SYS_RESOURCE haben — wir haben sie. */
    w = write_text("/proc/self/oom_adj", "-17\n", 4);
    check("write oom_adj=-17", w == 4);
    check_val("oom_score_adj == -1000", read_text_int("/proc/self/oom_score_adj"), -1000);
    check_val("oom_adj reads -17 back", read_text_int("/proc/self/oom_adj"), -17);

    /* +16 / -18 -> EINVAL */
    w = write_text("/proc/self/oom_adj", "16\n", 3);
    check_val("oom_adj +16 -> EINVAL", w, -EINVAL);
    w = write_text("/proc/self/oom_adj", "-18\n", 4);
    check_val("oom_adj -18 -> EINVAL", w, -EINVAL);

    /* Reset (auf 0 — geht nicht ohne caps weil min jetzt -1000, aber wir haben
     * die Cap). Need to use a fresh process to reset min. */
}
TEST("oom/adj_legacy", test_oom_adj_legacy);

/* ── /proc/$pid/oom_score sanity ─────────────────────
 *
 * Ein frischer Forked-Prozess hat geringe RSS — score sollte klein sein
 * mit adj=0 (basis-only). Mit adj=+1000 muss er >0 sein.
 * Mit adj=-1000 muss er 0 sein (immune). */

static void test_oom_score_sanity(void) {
    puts("\n[oom/score_sanity]\n");

    /* Ein neuer Prozess fuer saubere min-Bedingungen. */
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        long s_default = read_text_int("/proc/self/oom_score");
        if (s_default < 0 || s_default > 2000) sc1(SYS_EXIT_GROUP, 30);

        /* immune via -1000 */
        if (write_text("/proc/self/oom_score_adj", "-1000\n", 6) != 6)
            sc1(SYS_EXIT_GROUP, 31);
        long s_immune = read_text_int("/proc/self/oom_score");
        if (s_immune != 0) sc1(SYS_EXIT_GROUP, 32);

        /* Zurueck auf +1000 (CAP gewaehrt) — score muss >= s_default sein.
         * Achtung: nach -1000 ist min jetzt -1000; auf +1000 hochgehen
         * laesst min unveraendert (nur lowering aktualisiert min). */
        if (write_text("/proc/self/oom_score_adj", "1000\n", 5) != 5)
            sc1(SYS_EXIT_GROUP, 33);
        long s_high = read_text_int("/proc/self/oom_score");
        if (s_high < s_default) sc1(SYS_EXIT_GROUP, 34);
        if (s_high > 2000) sc1(SYS_EXIT_GROUP, 35);

        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }
    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check_val("score sanity (fork child)", WEXITSTATUS(status), 0);
}
TEST("oom/score_sanity", test_oom_score_sanity);

/* ── init (PID 1) protection: /proc/1/oom_score_adj == -1000 default-policy
 *
 * Linux: init (systemd/openrc) setzt sich selbst auf -1000 fruehzeitig.
 * Wir initialisieren init defaultmaessig auf 0 — der Test prueft nur, dass
 * PID 1 existiert und ein lesbares oom_score hat. Echte init-Immunitaet
 * wird in test_oom (LTP) durch out_of_memory()-Exklusion getestet. */

static void test_init_pid1_present(void) {
    puts("\n[oom/init_pid1]\n");

    /* /proc/1/oom_score_adj muss lesbar sein und im Range. */
    long fd = sc3(SYS_OPEN, (long)"/proc/1/oom_score_adj", O_RDONLY, 0);
    check("open /proc/1/oom_score_adj", fd >= 0);
    if (fd >= 0) {
        char buf[32] = {0};
        long r = sc3(SYS_READ, fd, (long)buf, 31);
        check("read pid1 score_adj", r > 0);
        sc1(SYS_CLOSE, fd);
    }

    /* /proc/1/oom_score lesbar. */
    long s = read_text_int("/proc/1/oom_score");
    check("pid1 oom_score >=0", s >= 0);
    check("pid1 oom_score <=2000", s <= 2000);
}
TEST("oom/init_pid1", test_init_pid1_present);

/* ── adj-reset on SUID exec: defensiver Pfad-Test ─────
 *
 * Wir haben kein SUID-Binary in der Test-Umgebung. Der Reset-Pfad in
 * process_exec.c triggert nur bei (S_ISUID && st_uid != ruid && !CAP_SYS_RESOURCE)
 * oder analog SGID. Indirekt: wir prueft dass ein execve() OHNE SUID-Bit
 * den Wert NICHT resetet (Inheritance-Pfad). */

static void test_adj_preserved_normal_exec(void) {
    puts("\n[oom/adj_preserved_normal_exec]\n");

    /* In aktuellem Prozess: setze 250, dann fork+exec -> erwarte 250 nach exec.
     * Aber unser Test-Binary hat kein argv[1] zum Lesen. Stattdessen nur
     * fork: child liest seinen adj nach setzen via parent. Inherit-Pfad ist
     * bereits in test_oom_score_adj_inherit getestet — dieser Test prueft
     * specifically execve-Pfad-Behaviour: wir koennen execve(/nonexistent)
     * machen, das EFAILt vor dem Reset-Hook und prueft nur, dass nichts
     * unerwartet zerstoert wird. */
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        if (write_text("/proc/self/oom_score_adj", "250\n", 4) != 4)
            sc1(SYS_EXIT_GROUP, 40);
        char *argv[2] = { (char *)"/nonexistent_oom", 0 };
        sc3(SYS_EXECVE, (long)argv[0], (long)argv, 0);
        /* execve muss -ENOENT liefern. adj bleibt 250. */
        long v = read_text_int("/proc/self/oom_score_adj");
        sc1(SYS_EXIT_GROUP, v == 250 ? 0 : 41);
        __builtin_unreachable();
    }
    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check_val("adj preserved over failed execve", WEXITSTATUS(status), 0);
}
TEST("oom/adj_preserved_normal_exec", test_adj_preserved_normal_exec);

/* Silence unused warning for kstrlen (used by fmt-helper compatibility). */
__attribute__((unused)) static void __unused_kstrlen(void) { (void)kstrlen; }
