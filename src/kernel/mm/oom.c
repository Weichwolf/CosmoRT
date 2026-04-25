/* CosmoRT OOM-Killer — Linux mm/oom_kill.c subset
 *
 * Bei pages_alloc/page_alloc-Fail unter normalen GFP-Bedingungen wird
 * out_of_memory() aufgerufen: scort alle PROC_ALIVE-Tasks via oom_badness()
 * und SIGKILLt das hoechst-bewertete Opfer. PID 1 (init) ist immun — wenn
 * sie der hoechst-bewertete Task waere, panic() statt kill (Linux-konform:
 * panic_on_oom oder ohne Pick).
 *
 * Score-Formel (oom_badness, Linux mm/oom_kill.c):
 *   pages    = mm-rss + swap + pgtables_bytes/PAGE_SIZE
 *   badness  = pages * 1000 / total_pages       (basis ~0..1000)
 *   adj_pts  = oom_score_adj * total_pages / 1000  (linear shift)
 *   score    = clamp(badness + adj_pts, 0, 2000)
 *   adj == OOM_SCORE_ADJ_MIN -> immune (return 0)
 *
 * /proc/$pid/oom_score liefert genau diesen Wert.
 *
 * Race-Modell: out_of_memory() laeuft synchron im allokierenden Task, also
 * unterbrechbar (kein Atomar-Kontext). Kandidatensuche unter pid_lock fuer
 * O(N) iteration ohne dass tasks freed werden zwischen Score und Kill.
 */

#include "proc/process.h"
#include "proc/proc_internal.h"
#include "mm/page_alloc.h"
#include "mm/vma.h"
#include "core/waitqueue.h"
#include "linux/signal.h"
#include "hw/serial.h"
#include "hal/hal.h"

extern long do_kill(int pid, int sig);

/* Resident pages eines Prozesses durch Page-Table-Walk.
 * Fuer Huge-Pages werden 512 4K-Pages gezaehlt (Linux file_rss + anon_rss
 * arbeiten in PAGE_SIZE-Einheiten, unabhaengig von Granularitaet). */
static uint64_t proc_rss_pages(process_t *p) {
    if (!p || !p->pml4) return 0;
    uint64_t rss = 0;
    uint64_t *pml4_t = p->pml4;
    for (int i = 0; i < 256; i++) {
        if (!(pml4_t[i] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4_t[i] & PTE_ADDR_MASK);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_PRESENT)) continue;
            uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[j] & PTE_ADDR_MASK);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT)) continue;
                if (pd[k] & PTE_PS) { rss += 512; continue; }
                uint64_t *pt = (uint64_t *)phys_to_virt(pd[k] & PTE_ADDR_MASK);
                for (int l = 0; l < 512; l++)
                    if (pt[l] & PTE_PRESENT) rss++;
            }
        }
    }
    return rss;
}

/* Pagetable-Pages: PML4 untere Haelfte (1) + jede PDPT/PD/PT die present
 * ist. Linux mm_pgtables_bytes(); Approximation reicht weil die Pages
 * weit kleiner als RSS bei realen Workloads bleiben. */
static uint64_t proc_pgtable_pages(process_t *p) {
    if (!p || !p->pml4) return 0;
    uint64_t n = 1; /* PML4 selbst */
    uint64_t *pml4_t = p->pml4;
    for (int i = 0; i < 256; i++) {
        if (!(pml4_t[i] & PTE_PRESENT)) continue;
        n++; /* PDPT */
        uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4_t[i] & PTE_ADDR_MASK);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_PRESENT)) continue;
            n++; /* PD */
            uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[j] & PTE_ADDR_MASK);
            for (int k = 0; k < 512; k++) {
                if ((pd[k] & PTE_PRESENT) && !(pd[k] & PTE_PS))
                    n++; /* PT */
            }
        }
    }
    return n;
}

unsigned int oom_badness(process_t *p, unsigned int total_pages) {
    if (!p || total_pages == 0) return 0;
    if (p->oom_score_adj <= OOM_SCORE_ADJ_MIN) return 0; /* immun */

    uint64_t pages = proc_rss_pages(p) + proc_pgtable_pages(p);

    /* badness = pages*1000/total + adj*total/1000. Doppelte Berechnung in
     * 64-bit damit kein Overflow bei adj=+1000 und total=2^28 Pages (1TB). */
    long basis = (long)((pages * 1000ULL) / (uint64_t)total_pages);
    long shift = ((long)p->oom_score_adj * (long)total_pages) / 1000L;
    long score = basis + shift;
    if (score < 0) score = 0;
    if (score > 2000) score = 2000;
    return (unsigned int)score;
}

/* Iter-Kontext fuer proc_for_each: trackt hoechst-scored victim. */
struct oom_pick {
    process_t   *victim;
    unsigned int top_score;
    unsigned int total_pages;
};

static int oom_score_iter(process_t *p, void *ctx) {
    struct oom_pick *pk = (struct oom_pick *)ctx;
    if (!p || p->state != PROC_ALIVE) return 0;
    /* Threads die schon SIGKILL pending haben sind im Sterben — auslassen,
     * sonst killen wir denselben Task zweimal und finden kein anderes Opfer. */
    if (p->sig_pending & SIG_BIT(SIGKILL)) return 0;
    unsigned int s = oom_badness(p, pk->total_pages);
    if (s == 0) return 0;
    /* Tie-Break: hoehere PID gewinnt (juenger -> weniger investiert).
     * Linux nimmt das erste Match — wir wollen Determinismus fuer Tests. */
    if (s > pk->top_score ||
        (s == pk->top_score && pk->victim && p->pid > pk->victim->pid)) {
        pk->top_score = s;
        pk->victim    = p;
    }
    return 0;
}

int out_of_memory(void) {
    unsigned int total_pages = (unsigned int)page_alloc_total();
    if (total_pages == 0) return 0;

    struct oom_pick pk = { .victim = 0, .top_score = 0, .total_pages = total_pages };
    proc_for_each(oom_score_iter, &pk);

    if (!pk.victim) {
        serial_puts("oom: no eligible victim — true ENOMEM\n");
        return 0;
    }

    /* PID 1 (init) ist immun — Linux: oom_kill_process bail-out + panic. */
    if (pk.victim->pid == 1) {
        serial_puts("oom: PANIC — would kill init (pid 1) — system unrecoverable\n");
        for (;;) hal_cpu_halt();
    }

    serial_puts("oom: killing pid ");
    {
        char tmp[16]; int n = 0; uint32_t v = pk.victim->pid;
        do { tmp[n++] = '0' + (char)(v % 10); v /= 10; } while (v);
        while (n--) serial_putchar(tmp[n]);
    }
    serial_puts(" score ");
    {
        char tmp[16]; int n = 0; unsigned int v = pk.top_score;
        do { tmp[n++] = '0' + (char)(v % 10); v /= 10; } while (v);
        while (n--) serial_putchar(tmp[n]);
    }
    serial_puts(" (");
    {
        const char *c = pk.victim->comm[0] ? pk.victim->comm : "?";
        while (*c) serial_putchar(*c++);
    }
    serial_puts(")\n");

    /* SIGKILL via standard kill_one. Threads werden THREAD_DEAD, exit-Pfad
     * gibt Pages zurueck. Caller schlaeft kurz und retried alloc einmal. */
    do_kill((int)pk.victim->pid, SIGKILL);
    return 1;
}
