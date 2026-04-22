/* CosmoRT dnotify — Legacy Directory-Watch ueber fcntl(F_NOTIFY).
 *
 * Linux fs/notify/dnotify/dnotify.c: pro Directory-Watch hängt eine
 * dnotify_struct an der Inode. Jede Mutation im Verzeichnis feuert
 * das konfigurierte Signal (default SIGIO) an den Besitzer.
 *
 * Wir halten eine globale Liste slab-allozierter Watches. Mutations-
 * Hooks (vfs_chmod, vfs_rename, vfs_unlink, vfs_create, vfs_mkdir,
 * vfs_rmdir) rufen dnotify_fire(parent_abspath, mask) auf. Der Watch
 * matcht nur Events, die unmittelbar im gewatchten Verzeichnis
 * stattfinden (DN_MULTISHOT sorgt fuer dauerhafte Aktivitaet).
 *
 * SA_SIGINFO-Delivery: si_fd kommt aus der per-Process-dnotify_q
 * (siehe signal_frame.c:deliver_signal → process->dnotify_q_*).
 */

#include "proc/process.h"
#include "proc/thread.h"
#include "mm/slab.h"
#include "linux/fcntl.h"
#include "linux/signal.h"
#include "linux/errno.h"
#include "spinlock.h"

struct dnotify_watch {
    struct dnotify_watch *next;
    process_t *p;
    int        fd;
    uint32_t   mask;        /* DN_ACCESS | DN_MODIFY | ... (ohne DN_MULTISHOT-Bit) */
    int        multishot;   /* 1 = DN_MULTISHOT gesetzt */
    int        sig;         /* 0 = default SIGIO */
    char       abspath[256];/* absolutes Directory-Pfad */
};

static slab_t dnotify_slab;
static int    dnotify_slab_inited;
static struct dnotify_watch *dn_head;
static spinlock_t dn_lock = SPINLOCK_INIT;

static void dn_slab_ensure(void) {
    if (__builtin_expect(dnotify_slab_inited, 1)) return;
    slab_init_dynamic(&dnotify_slab, (int)sizeof(struct dnotify_watch), 4);
    dnotify_slab_inited = 1;
}

static int path_copy(char *dst, const char *src, int maxlen) {
    int i = 0;
    for (; i < maxlen - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
    return i;
}

/* Match-Semantik:
 *   allow_self=1: DN_ATTRIB-artige Events, bei denen Linux auch Watches auf
 *                 der Inode selbst triggert (fsnotify(inode, FS_ATTRIB)).
 *   allow_self=0: DN_CREATE/DELETE/RENAME — Linux feuert nur fuer Parent-
 *                 Watches (fsnotify_parent). */
static int dn_path_matches(const char *watch, const char *event, int allow_self) {
    int wn = 0; while (watch[wn]) wn++;
    if (wn > 1 && watch[wn - 1] == '/') wn--;
    int i = 0;
    for (; i < wn; i++) if (watch[i] != event[i]) return 0;
    char c = event[i];
    if (c == 0) return allow_self;
    if (c != '/') return 0;
    i++;
    for (; event[i]; i++)
        if (event[i] == '/') return 0;
    return 1;
}

/* Push {sig, fd} in prozess-eigene dnotify-queue. Wird in deliver_signal
 * ausgelesen, damit si_fd pro Signal stimmt. */
static void dn_queue_push(process_t *p, int sig, int fd) {
    int next = (p->dnotify_q_tail + 1) & 15;
    if (next == p->dnotify_q_head) return; /* voll — verwerfen wie Linux */
    p->dnotify_q_sig[p->dnotify_q_tail] = sig;
    p->dnotify_q_fd [p->dnotify_q_tail] = fd;
    p->dnotify_q_tail = next;
}

/* Peek: gibt es noch einen Eintrag fuer diesen sig? Kein Pop — nur Prueffen,
 * damit deliver_signal nach Pop entscheiden kann, ob sig erneut pending bleibt
 * (RT-Signal-Queue-Semantik in 1-Bit-sig_pending nachbilden). */
int dnotify_queue_peek_fd(process_t *p, int sig) {
    if (!p) return -1;
    int h = p->dnotify_q_head;
    while (h != p->dnotify_q_tail) {
        if (p->dnotify_q_sig[h] == sig) return p->dnotify_q_fd[h];
        h = (h + 1) & 15;
    }
    return -1;
}

/* Fuer deliver_signal: nehme den ersten Eintrag mit passendem sig. -1 wenn
 * keiner — Caller faellt dann auf Default zurueck. */
int dnotify_queue_pop_fd(process_t *p, int sig) {
    if (!p) return -1;
    if (p->dnotify_q_head == p->dnotify_q_tail) return -1;
    /* Lineare Suche nach passendem sig, Eintrag rausziehen (FIFO-Order
     * bleibt intakt, Luecken werden zusammengeschoben). */
    int h = p->dnotify_q_head;
    while (h != p->dnotify_q_tail) {
        if (p->dnotify_q_sig[h] == sig) {
            int fd = p->dnotify_q_fd[h];
            /* Compact */
            int i = h;
            while (i != p->dnotify_q_tail) {
                int n = (i + 1) & 15;
                if (n == p->dnotify_q_tail) break;
                p->dnotify_q_sig[i] = p->dnotify_q_sig[n];
                p->dnotify_q_fd [i] = p->dnotify_q_fd [n];
                i = n;
            }
            p->dnotify_q_tail = (p->dnotify_q_tail - 1) & 15;
            return fd;
        }
        h = (h + 1) & 15;
    }
    return -1;
}

extern long do_kill(int pid, int sig);

/* fcntl F_NOTIFY — Wert: DN_* Maske mit optionalem DN_MULTISHOT.
 * arg == 0 und MULTISHOT unset -> Watch entfernen.
 * Mehrfach-Call mit MULTISHOT mergt Maske (Linux-Semantik). */
long dnotify_ctl(int fd, const char *abspath, uint32_t arg, int sig) {
    uint32_t mask = arg & 0x3F; /* DN_ACCESS..DN_ATTRIB */
    int multishot = (arg & DN_MULTISHOT) ? 1 : 0;

    process_t *p = proc_current();
    if (!p) return -EFAULT;

    uint64_t flags;
    spin_lock_irq(&dn_lock, &flags);
    /* Suche existierenden Watch fuer (p, fd) */
    struct dnotify_watch **pp = &dn_head, *w = 0;
    while (*pp) {
        if ((*pp)->p == p && (*pp)->fd == fd) { w = *pp; break; }
        pp = &(*pp)->next;
    }

    if (arg == 0) {
        /* remove */
        if (w) { *pp = w->next; slab_free(&dnotify_slab, w); }
        spin_unlock_irq(&dn_lock, flags);
        return 0;
    }

    if (w) {
        w->mask |= mask;
        if (multishot) w->multishot = 1;
        if (sig) w->sig = sig;
        spin_unlock_irq(&dn_lock, flags);
        return 0;
    }

    dn_slab_ensure();
    w = (struct dnotify_watch *)slab_alloc(&dnotify_slab);
    if (!w) { spin_unlock_irq(&dn_lock, flags); return -ENOMEM; }
    w->next = dn_head; dn_head = w;
    w->p = p;
    w->fd = fd;
    w->mask = mask;
    w->multishot = multishot;
    w->sig = sig;
    path_copy(w->abspath, abspath, (int)sizeof(w->abspath));
    spin_unlock_irq(&dn_lock, flags);
    return 0;
}

void dnotify_fd_closed(process_t *p, int fd) {
    if (!p) return;
    uint64_t flags;
    spin_lock_irq(&dn_lock, &flags);
    struct dnotify_watch **pp = &dn_head;
    while (*pp) {
        if ((*pp)->p == p && (*pp)->fd == fd) {
            struct dnotify_watch *dead = *pp;
            *pp = dead->next;
            slab_free(&dnotify_slab, dead);
            continue;
        }
        pp = &(*pp)->next;
    }
    spin_unlock_irq(&dn_lock, flags);
}

void dnotify_proc_exit(process_t *p) {
    if (!p) return;
    uint64_t flags;
    spin_lock_irq(&dn_lock, &flags);
    struct dnotify_watch **pp = &dn_head;
    while (*pp) {
        if ((*pp)->p == p) {
            struct dnotify_watch *dead = *pp;
            *pp = dead->next;
            slab_free(&dnotify_slab, dead);
            continue;
        }
        pp = &(*pp)->next;
    }
    spin_unlock_irq(&dn_lock, flags);
}

/* Feuere dnotify-Events. event_path ist der absolute Pfad der betroffenen
 * Inode (z.B. umbenannte Datei) — Watches auf Parent sowie auf Event-
 * Pfad selbst (DN_ATTRIB auf Directory) werden gematcht. */
void dnotify_fire(const char *event_path, uint32_t mask) {
    if (!event_path) return;
    int allow_self = (mask & DN_ATTRIB) ? 1 : 0;
    struct {
        process_t *p;
        int sig;
        int fd;
    } pending[32];
    int np = 0;

    uint64_t flags;
    spin_lock_irq(&dn_lock, &flags);
    for (struct dnotify_watch *w = dn_head; w && np < 32; w = w->next) {
        if (!(w->mask & mask)) continue;
        if (!dn_path_matches(w->abspath, event_path, allow_self)) continue;
        pending[np].p = w->p;
        pending[np].sig = w->sig ? w->sig : 29 /* SIGIO */;
        pending[np].fd = w->fd;
        np++;
        if (!w->multishot) w->mask = 0; /* one-shot verbraucht */
    }
    /* queue siginfo-Daten pro Prozess waehrend wir noch den Lock halten */
    for (int i = 0; i < np; i++)
        dn_queue_push(pending[i].p, pending[i].sig, pending[i].fd);
    spin_unlock_irq(&dn_lock, flags);

    /* Signals ausserhalb des Locks ausliefern */
    for (int i = 0; i < np; i++)
        do_kill((int)pending[i].p->pid, pending[i].sig);
}
