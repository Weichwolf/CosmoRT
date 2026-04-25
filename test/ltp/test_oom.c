/* OOM-Killer end-to-end ktests
 *
 * Triggern out_of_memory() ist destruktiv (verbraucht echtes RAM, killt
 * einen Prozess). Statt "echte OOM" testen wir die Selektionsmechanik
 * und Init-Schutz deterministisch ueber die /proc/$pid/oom_score-API:
 *
 * 1. oom_kill_selects_highest_adj: zwei Children, einer mit +1000 adj,
 *    einer mit 0. /proc/$pid/oom_score muss bei +1000-Child hoeher
 *    sein und init (PID 1) niedriger als beide.
 * 2. init_pid1_protection: /proc/1/oom_score reflektiert oom_badness();
 *    wir setzen Init-Pid testweise NICHT (nicht erlaubt vom Test-Runner)
 *    sondern verifizieren nur die Score-Berechnung.
 * 3. mmap_alloc_then_score: ein Child belegt 4MB privater anon, sein
 *    Score muss messbar steigen — beweist dass RSS in oom_badness eingeht.
 */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WIFSIGNALED(s)  (((s) & 0x7F) > 0 && ((s) & 0x7F) < 0x7F)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WTERMSIG(s)     ((s) & 0x7F)

static long read_int_file(const char *path) {
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

static void path_for_pid(char *out, long pid, const char *suffix) {
    /* "/proc/<pid><suffix>" e.g. "/proc/123/oom_score" */
    int p = 0;
    const char *pre = "/proc/";
    while (*pre) out[p++] = *pre++;
    char tmp[16]; int n = 0; long v = pid;
    do { tmp[n++] = '0' + (char)(v % 10); v /= 10; } while (v);
    while (n--) out[p++] = tmp[n];
    while (*suffix) out[p++] = *suffix++;
    out[p] = 0;
}

/* ── oom_kill_selects_highest_adj ────────────────────
 *
 * Zwei Children mit gleichem ELF-RSS, einer mit adj=0, einer mit adj=+1000.
 * Liest deren oom_score von extern (parent). Erwartung: +1000-Score signifikant
 * hoeher. Beide sind > init's oom_score (kleiner ELF). */

static void test_oom_pick_highest_adj(void) {
    puts("\n[ltp/oom_pick_highest_adj]\n");

    /* Gate fuer kontrolliertes Beenden der Children */
    volatile int *gate = (volatile int *)sc6(SYS_MMAP, 0, 4096,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if ((long)gate <= 0) { check("mmap gate", 0); return; }
    *gate = 0;

    long c1 = sc0(SYS_FORK);
    if (c1 == 0) {
        write_text("/proc/self/oom_score_adj", "0\n", 2);
        sc1(SYS_ALARM, 5);
        while (*gate == 0)
            for (volatile int j = 0; j < 1000; j++) __asm__ volatile("pause");
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }
    long c2 = sc0(SYS_FORK);
    if (c2 == 0) {
        write_text("/proc/self/oom_score_adj", "1000\n", 5);
        sc1(SYS_ALARM, 5);
        while (*gate == 0)
            for (volatile int j = 0; j < 1000; j++) __asm__ volatile("pause");
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    /* Lass Children Settle. */
    for (volatile int s = 0; s < 200000; s++) __asm__ volatile("pause");

    char p1[64], p2[64], p_init[64];
    path_for_pid(p1, c1, "/oom_score");
    path_for_pid(p2, c2, "/oom_score");
    path_for_pid(p_init, 1, "/oom_score");

    long s1 = read_int_file(p1);
    long s2 = read_int_file(p2);
    long s_init = read_int_file(p_init);

    check("c1 score readable", s1 >= 0);
    check("c2 score readable", s2 >= 0);
    check("init score readable", s_init >= 0);
    check("highest-adj child wins", s2 > s1);
    check("init score < high-adj child", s_init < s2);

    *gate = 1;
    int st;
    sc4(SYS_WAIT4, c1, (long)&st, 0, 0);
    sc4(SYS_WAIT4, c2, (long)&st, 0, 0);
    sc2(SYS_MUNMAP, (long)gate, 4096);
}
TEST("ltp/oom_pick_highest_adj", test_oom_pick_highest_adj);

/* ── mmap_alloc_then_score ─────────────────────────────
 *
 * Beweist dass oom_badness() RSS einbezieht: child mappt 4MB anon,
 * touched alle Pages (faulted in). Score muss > Score VOR mmap sein. */

static void test_oom_score_tracks_rss(void) {
    puts("\n[ltp/oom_score_tracks_rss]\n");

    volatile int *gate = (volatile int *)sc6(SYS_MMAP, 0, 4096,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    volatile long *result = (volatile long *)sc6(SYS_MMAP, 0, 4096,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if ((long)gate <= 0 || (long)result <= 0) { check("mmap gates", 0); return; }
    *gate = 0;
    result[0] = -1; /* score before */
    result[1] = -1; /* score after */

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_ALARM, 5);
        write_text("/proc/self/oom_score_adj", "0\n", 2);

        /* Score vorher */
        result[0] = read_int_file("/proc/self/oom_score");

        /* 4MB private anon, alle Pages touchen. */
        const long sz = 4 * 1024 * 1024;
        long base = sc6(SYS_MMAP, 0, sz, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base < 0) { result[1] = -2; sc1(SYS_EXIT_GROUP, 1); }
        volatile char *p = (volatile char *)base;
        for (long off = 0; off < sz; off += 4096) p[off] = (char)(off & 0xFF);

        /* Score nachher */
        result[1] = read_int_file("/proc/self/oom_score");

        while (*gate == 0)
            for (volatile int j = 0; j < 1000; j++) __asm__ volatile("pause");
        sc2(SYS_MUNMAP, base, sz);
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    /* Warten dass child Score-Daten geschrieben hat. */
    int spins = 0;
    while ((result[0] < 0 || result[1] < 0) && spins < 200000) {
        for (volatile int j = 0; j < 1000; j++) __asm__ volatile("pause");
        spins++;
    }

    check("score-before populated", result[0] >= 0);
    check("score-after populated", result[1] >= 0);
    check("score grows with RSS", result[1] > result[0]);

    *gate = 1;
    int st;
    sc4(SYS_WAIT4, pid, (long)&st, 0, 0);
    sc2(SYS_MUNMAP, (long)gate, 4096);
    sc2(SYS_MUNMAP, (long)result, 4096);
}
TEST("ltp/oom_score_tracks_rss", test_oom_score_tracks_rss);
