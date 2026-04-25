# Modernes Kernel-Design für CosmoRT (Stand 2026)

Architektur-Recherche und Recommendations. Linus-today-Geist: das *beste* Design,
nicht das Linux-Design. ABI bleibt Linux x86_64 — Internas duerfen modern sein.

Ausloeser: Phase 10.2 Waitqueue-Migration scheitert reproduzierbar an einer
subtilen Architektur-Verletzung in `sched_wake`. Dieses Dokument benennt die
Wurzel, vergleicht mit Linux/seL4/Zircon/io_uring und gibt eine konkrete
Migrations-Sequenz.

---

## 1. Executive Summary

| # | Empfehlung | Hebel |
|---|------------|-------|
| 1 | `sched_wake` darf NICHT mehr ueber `wait_head` routen. State-CAS auf `task->state` macht den Wake. Waitqueue ist nur Callback-Liste, kein Routing-Target. | Phase 10.2 Blockade aufloesen, Signal/Timer/IO-Wakeups orthogonal. |
| 2 | Eine Wake-Funktion: `try_to_wake_up(task, state_mask)`. Alle anderen (`sched_wake`, `event_post`, `wake_up`, `signal_wake_up`) sind Wrapper. | Race-Audit-Surface von ~14 Pfaden auf 1. |
| 3 | `event_queue` aus `thread_t` entfernen. Per-Subsystem-Waitqueue + `prepare_to_wait` ist die einzige Block-Mechanik. `event_post` wird Wrapper. | ~280 Zeilen `event_queue.c` weg. Race-Klasse "stale events" verschwindet. |
| 4 | Signal-Wake ist ein State-Bit (`TIF_SIGPENDING`-Aequivalent), kein Wakeup. Wake setzt zusaetzlich Run-State, Routing entfaellt. | sigtimedwait/futex/epoll alle korrekt unterbrechbar ohne Sonderpfade. |
| 5 | Interne Audio-API auf io_uring-aehnlichem Completion-Modell statt epoll. epoll bleibt fuer ABI. UMWAIT fuer Sub-µs-Wakeups in Phase 19. | Sub-ms-Latenz erreichbar; epoll als ABI-Wrapper bleibt korrekt. |

---

## 2. Task-State-Machine — die eigentliche Wurzel

### 2.1 Status quo CosmoRT

`thread_t.state` (in `include/kernel/proc/thread.h:82`):
```
THREAD_FREE / RUNNABLE / RUNNING / BLOCKED / DEAD / STOPPED
```

`thread_t.wait_head` ist `void*` und wird in `prepare_to_wait()` gesetzt
(`waitqueue.c:97`). `sched_wake()` (`sched.c:83-110`) macht:

```c
wait_queue_head_t *wh = atomic_load(&t->wait_head);
if (wh) { wake_up_all(wh); return; }       // (A) routing!
old = CAS(&t->state, BLOCKED, RUNNABLE);   // (B) direct path
if (old == BLOCKED) sched_add(t);
```

**Das ist der Bug**. Pfad (A) wuenscht sich:
"Thread parkt auf wq → Waker, der das nicht weiss, soll das richtige tun".
Was er tatsaechlich tut: jeder Aufrufer von `sched_wake` (Signal-Delivery,
Timer-Expiry, kill_one, exit_notify, …) ruft auf einer fremden Waitqueue
`wake_up_all` — auch wenn die Bedingung dieser Waitqueue unerfuellt ist.

**Konsequenzen** (alle empirisch beobachtet):
- `nanosleep01` haengt: Signal kommt waehrend Sleep auf eigener wq. `sched_wake`
  ruft `wake_up_all` der Sleep-Wq → Thread wacht, sleep_interruptible_ns
  klassifiziert als "spurious", schlaeft erneut. Signal nie geliefert.
- `pthread_cond` flaky: Condvar parkt auf futex-Hashbucket-wq. `kill` (oder
  Timer) → wake_up_all auf der falschen wq → Wakeup bei korrektem futex_wake
  rennt gegen "schon wach durch fremden Wake".
- `tls_init`/`sem_init`-Deadlocks: gemischter Pfad (event_queue + waitqueue
  gleichzeitig) — `t->wait_head` gesetzt, `t->eq` ebenfalls verwendet → Wake
  geht an wq, event_queue wartet weiter.

### 2.2 Linux-Modell (faktisch)

```
struct task_struct {
    unsigned int  __state;       // RUNNING / INTERRUPTIBLE / UNINTERRUPTIBLE / ...
    unsigned long thread_info.flags;  // TIF_SIGPENDING, TIF_NEED_RESCHED, ...
    /* keine wait_head/wait_entry pointer */
};
```

Die zentrale Funktion (`kernel/sched/core.c::try_to_wake_up`) macht schematisch:

```c
int try_to_wake_up(struct task_struct *p, unsigned int state_mask, int flags) {
    raw_spin_lock_irqsave(&p->pi_lock, flags);
    if (!(p->__state & state_mask)) goto out;
    /* p IS in a sleep state we accept */
    p->__state = TASK_WAKING;
    /* enqueue on rq + smp_send_reschedule wenn nicht hier */
    ttwu_queue(p, cpu);
out:
    raw_spin_unlock_irqrestore(...);
}
```

Schluesselpunkte:

| Eigenschaft | Wert | Konsequenz |
|-------------|------|------------|
| Routing-Target | rq (per-CPU runqueue), niemals waitqueue | Waker kennt Waitqueue nicht |
| Lock | `task->pi_lock` (per-Task) | Skalierbar, kein globales Routing-Schloss |
| State-Filter | `state_mask` Argument | `wake_up_state(t, TASK_INTERRUPTIBLE)` weckt nur, wenn Thread interruptible parkt |
| Waitqueue | nur Callback-Liste mit `wq_entry->func` | wake_up iteriert Liste und ruft `func(entry, mode, ...)` |
| `func` default | `default_wake_function` ⇒ `try_to_wake_up(entry->private, mode, 0)` | Trennung: WQ haelt Callbacks, Wake macht State |

Bedeutung: **die Waitqueue weiss nicht, wer drauf schlaeft, sie ruft nur
Callbacks. Der Callback weiss, welchen Task er weckt.** Damit ist Wake total
entkoppelt vom Routing — Signal-Delivery ruft `signal_wake_up(p, resume)`
direkt, ohne irgendeine Waitqueue zu beruehren.

### 2.3 Empfehlung CosmoRT

**Drei diskrete Schritte:**

```c
/* Schritt 1: state-Mask fuer try_to_wake_up */
#define TASK_RUNNING        0x0000
#define TASK_INTERRUPTIBLE  0x0001
#define TASK_UNINTERRUPTIBLE 0x0002
#define TASK_STOPPED        0x0004
#define TASK_KILLABLE       (TASK_INTERRUPTIBLE | __TASK_KILLABLE)
#define TASK_NORMAL         (TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)

/* Schritt 2: einzige Wake-Primitive */
int try_to_wake_up(thread_t *t, unsigned int mask);
/* CAS p->state bei state & mask, sched_add. Sonst no-op. */

/* Schritt 3: wait_head/wait_entry aus thread_t ENTFERNEN */
```

Waitqueue wird:
```c
struct wait_queue_entry {
    unsigned int flags;
    void        *priv;       /* zumeist thread_t* */
    int        (*func)(struct wait_queue_entry *, unsigned int mask);
    struct list_head list;
};
```

`wake_up(wq)` iteriert und ruft `e->func(e, TASK_NORMAL)`. Default-Func ist
`autoremove_wake_function` → `try_to_wake_up(e->priv, mask)`. epoll-Aequivalent
(siehe §3) registriert eigene Funktion `ep_poll_callback`.

**Migration aus `sched_wake`**:
- `sched_wake(t)` wird zu `try_to_wake_up(t, TASK_NORMAL)`.
- `wait_head`-Loesch-Branch ENTFAELLT komplett.
- THREAD_BLOCKED wird durch das Maskenmodell ersetzt — Mapping:
  - `THREAD_BLOCKED` + `interruptible` → `TASK_INTERRUPTIBLE`
  - `THREAD_BLOCKED` ohne signal-Pfad → `TASK_UNINTERRUPTIBLE`

Quelle Linux: `kernel/sched/core.c::try_to_wake_up`,
`include/linux/sched.h` (TASK_*-Bits).

---

## 3. Waitqueue-Design

### 3.1 Status quo CosmoRT

`waitqueue.c` ist gut bauplangetreu (`prepare_to_wait` → `schedule` →
`finish_wait`), aber `bind_wait`/`unbind_wait` setzen `t->wait_head`. Das
schliesst den Race lokal, oeffnet aber den Routing-Bug global (§2).

### 3.2 Linux ep_poll_callback exakt

Strukturen (`fs/eventpoll.c`):
```c
struct eventpoll {
    struct mutex          mtx;        /* eventfd-Liste, copy_to_user */
    wait_queue_head_t     wq;         /* hier schlafen epoll_wait-Caller */
    wait_queue_head_t     poll_wait;  /* fuer nested epoll-on-epoll */
    struct list_head      rdllist;    /* ready-Liste */
    rwlock_t              lock;       /* schuetzt rdllist + ovflist */
    struct rb_root_cached rbr;        /* registrierte fds */
    struct epitem        *ovflist;    /* overflow waehrend send_events laeuft */
};

struct epitem {
    struct rb_node              rbn;
    struct list_head            rdllink;     /* in ep->rdllist */
    struct eppoll_entry        *pwqlist;     /* eintraege in fd-Waitqueues */
    struct epoll_event          event;       /* user-data */
};

struct eppoll_entry {
    wait_queue_entry_t  wait;        /* func = ep_poll_callback */
    wait_queue_head_t  *whead;       /* fd's eigene wq */
    struct epitem      *base;        /* zurueck zur epitem */
};
```

Lock-Hierarchie (Acquisition Order strikt 1→2→3):
```
1. epnested_mutex   (global, nur fuer Nesting-Cycle-Detection)
2. ep->mtx          (per-eventpoll, sleeping)
3. ep->lock         (per-eventpoll, IRQ-safe spinlock)
```

**Wakeup-Pfad** (Device IRQ → User):
```
1. NIC-IRQ in tcp.c     ⇒ wake_up(&sock->sk_wq)
2. wake_up iteriert wq  ⇒ ruft eppoll_entry.wait.func == ep_poll_callback(...)
3. ep_poll_callback:
     read_lock(&ep->lock)        // rwlock! Nur Lesen, viele Callbacks parallel
     list_add_tail(&epi->rdllink, &ep->rdllist)
     wake_up_locked(&ep->wq)     // epoll_wait-Sleeper
     read_unlock(&ep->lock)
4. Sleeping epoll_wait wacht auf, ep_send_events kopiert rdllist nach user
```

**Schluesselbeobachtungen**:
- ep_poll_callback ist NICHT auf t->wait_head angewiesen. wait_queue_entry hat
  ein `func`-Feld; epoll uebersteuert es — fertig. CosmoRT braucht das selbe.
- rwlock seit Linux 5.1 ([commit a218cc4914](https://github.com/torvalds/linux/commit/a218cc4914209ac14476cb32769b31a556355b22)):
  Reader-Lock fuer ep_poll_callback, Writer-Lock fuer Listen-Mutationen.
  Mehrere fd-IRQs koennen parallel epoll-eintragen.
- `ovflist` deckt das Race "send_events laeuft mit ep->mtx, IRQ kommt"
  ab — neue Ready-Items landen in `ovflist`, werden nach send_events in
  `rdllist` geflusht.

### 3.3 Empfehlung CosmoRT

```c
typedef int (*wait_func_t)(struct wait_queue_entry *, unsigned int mask);

struct wait_queue_entry {
    unsigned int       flags;
    void              *priv;
    wait_func_t        func;
    struct list_head   node;
};

#define DEFINE_WAIT(name) \
    struct wait_queue_entry name = { \
        .priv = thread_current(), \
        .func = autoremove_wake_function, \
        .node = LIST_HEAD_INIT(name.node), \
    }

int autoremove_wake_function(struct wait_queue_entry *e, unsigned int mask) {
    int r = try_to_wake_up(e->priv, mask);
    if (r) list_del_init(&e->node);
    return r;
}
```

`wake_up(wq, mask, nr_exclusive)` iteriert die Liste, ruft `e->func(e, mask)`,
zaehlt erfolgreiche Wakes.

**epoll-Aequivalent** in CosmoRT: `epitem.entry.func = ep_poll_callback`,
`ep_poll_callback` greift `ep->lock`, fuegt in `rdllist` ein, ruft selbst
`__wake_up_locked(&ep->wq, TASK_INTERRUPTIBLE, 1)`.

**Damit verschwindet** das Per-Core-Sleeper-Array in `epoll.c:230-272`
ersatzlos. Ebenso die `epoll_wake_all`-Funktion und der `wake_at_tsc`-Hack.

---

## 4. Signal-Wake Architektur

### 4.1 Status quo

`signal.c::kill_one` (Zeile 354-411) hat fuer JEDEN Pfad doppelte Logik:

```c
if (t->wait_head)
    sched_wake(t);                 /* wuerde wake_up_all routen */
else
    event_post(t, EQ_CHILD_EXITED, sig);  /* legacy event_queue */
```

Das ist Symptom, nicht Ursache. Der wirkliche Defekt: Signal-Delivery hat keinen
*einheitlichen* Mechanismus — sie versucht "den richtigen Wake-Pfad zu raten".

### 4.2 Linux-Modell

```
1. send_signal(t, sig):
     spin_lock(&t->sighand->siglock);
     sigaddset(&t->pending.signal, sig);
     signal_wake_up(t, sig == SIGKILL);
     spin_unlock(...);

2. signal_wake_up(t, resume):
     set_tsk_thread_flag(t, TIF_SIGPENDING);
     state_mask = TASK_INTERRUPTIBLE;
     if (resume) state_mask |= TASK_WAKEKILL;  /* SIGKILL kann auch UNINT wecken */
     try_to_wake_up(t, state_mask);

3. Beim Return-to-User pruefen alle Syscall-Pfade TIF_SIGPENDING.
4. Beim try_to_wake_up:
     - State-Match? CAS state → TASK_RUNNING, sched_add.
     - Kein Match? No-Op (sigpending bit bleibt, naechster schedule sieht's).
```

**Kritischer Punkt**: `signal_wake_up` ruft `try_to_wake_up` direkt, *nie*
ueber eine Waitqueue. Die Waitqueue (futex-bucket / epoll-wq / pipe-wq), auf
der der Thread parkt, wird NIE vom Signal-Pfad beruehrt.

Wie kommt der Thread dann aus seiner Waitqueue?

**Antwort**: `wait_event_interruptible` ist ein Loop:
```c
#define wait_event_interruptible(wq, cond) ({               \
    DEFINE_WAIT(__wait);                                    \
    int __ret = 0;                                          \
    for (;;) {                                              \
        prepare_to_wait(&wq, &__wait, TASK_INTERRUPTIBLE);  \
        if (cond) break;                                    \
        if (signal_pending(current)) { __ret = -ERESTARTSYS; break; } \
        schedule();                                         \
    }                                                       \
    finish_wait(&wq, &__wait);                              \
    __ret;                                                  \
})
```

Der Loop pruefte: cond? signal? schedule. `try_to_wake_up` aus Signal-Pfad
setzt RUNNING — schedule kehrt zurueck — Loop checkt signal_pending — return
-ERESTARTSYS. **Die Waitqueue selbst hat damit nichts zu tun.**

`finish_wait` raeumt den Listeneintrag auf, niemand musste ihn vom Signal-Pfad
finden.

### 4.3 Empfehlung CosmoRT

```c
/* Sequenz-Diagramm: kill(pid, SIGTERM) waehrend pthread_cond_wait */
                                                                      
Thread T parkt auf futex-bucket-wq:                                  
  prepare_to_wait(&bucket->wq, &entry, TASK_INTERRUPTIBLE)            
  → state = TASK_INTERRUPTIBLE                                        
  → entry in bucket->wq.list                                          
  → schedule() → context_switch                                       
                                                                      
kill_one(T, SIGTERM):                                                 
  spin_lock(&T->sighand->siglock)                                     
  T->pending |= SIG_BIT(SIGTERM)                                      
  set_thread_flag(T, TIF_SIGPENDING)                                  
  try_to_wake_up(T, TASK_INTERRUPTIBLE):                              
    spin_lock(&T->pi_lock)                                            
    if (T->state & TASK_INTERRUPTIBLE) {                              
      T->state = TASK_RUNNING                                         
      sched_add(T)                                                    
    }                                                                 
    spin_unlock(&T->pi_lock)                                          
  spin_unlock(&T->sighand->siglock)                                   
                                                                      
T scheduled wieder → schedule() returns →                             
futex_wait loop checkt signal_pending → returns -ERESTARTSYS          
→ syscall layer schreibt -EINTR oder restart                          
→ deliver_signal(T) baut sigframe, springt nach Handler               
                                                                      
Wichtig: bucket->wq.list haelt entry bis finish_wait. Niemand          
ausser T selbst beruehrt die wq aus dem Signal-Pfad.                  
```

**Konsequenzen fuer Code**:
- `kill_one`-Doppelpfad `if (t->wait_head) sched_wake; else event_post` weg.
- `signal_wake_up(t)` ruft `try_to_wake_up(t, TASK_INTERRUPTIBLE | TASK_WAKEKILL)`.
- `event_post(t, EQ_CHILD_EXITED, sig)` als "Wake-Mechanismus" entfaellt;
  EQ_CHILD_EXITED bleibt nur als wait4-Notification, wenn das ein eigener
  Subsystem-Wakeup ist.

`signal_pending(t)` wird:
```c
static inline int signal_pending(thread_t *t) {
    if (!t->proc) return 0;
    return ((t->proc->sig_pending | t->sig_thread_pending)
            & ~t->sig_blocked) != 0;
}
```

(Existiert in `waitqueue.c::signal_deliverable` schon — umbenennen, in alle
Wait-Loops einsetzen.)

---

## 5. Async-IO API: epoll vs io_uring vs kqueue

### 5.1 Vergleich

| Eigenschaft | epoll | kqueue | io_uring |
|-------------|-------|--------|----------|
| Modell | Readiness | Readiness + Filter | Completion |
| Syscalls/op (typ.) | 1 ctl + 1 wait | 1 kevent | 0 (SQPOLL) - 1 (enter) |
| Multi-op-batch | Nein | Ja (kevent[]) | Ja (SQ ring) |
| File regulars | Nein (-EPERM) | Ja | Ja |
| Edge/Level | beide (komplex) | beide (sauber) | n/a (Completion) |
| Cancel laufender op | Nein | Nein | Ja (IORING_OP_ASYNC_CANCEL) |
| Multishot | Nein | Native | Ja (5.19+) |
| Sleep-State Hot-Path | syscall + ctxsw | syscall + ctxsw | UMWAIT/none mit SQPOLL |

### 5.2 Audio-RT-Bewertung

Sub-ms-Audio braucht:
- Sub-µs Wakeup-Latenz auf Period-Grenze (256 Frames @ 48k = 5.3ms; 64 = 1.3ms).
- **Keine** Syscall im Steady-State.
- Backpressure-fest (DMA-Buffer-Underrun = harte Audio-Aussetzer).

**io_uring SQPOLL** trifft das exakt:
- Kernel-Thread polled SQ ohne IRQ. Userspace schreibt SQE per MMIO-Style-Store.
- Completion via CQ ring, ebenfalls memorymapped.
- Mit `IORING_FEAT_FAST_POLL` (5.7+) plus `IORING_OP_READ_MULTISHOT` (5.19+):
  Audio-Producer schickt einen READ_MULTISHOT auf eine Audio-Pipe, bekommt
  jeden Frame als CQ-Entry — *null* Syscalls im Steady State.

**epoll** braucht im Vergleich pro Period: 1× `epoll_wait` + 1× `read` = 2
Syscalls + 2 Context-Switches. Bei 1.3ms Period und 5µs Syscall-Cost ist das
~0.8% Overhead — nicht katastrophal, aber Konkurrenten (FreeBSD/JACK auf
io_uring-Variante) sind besser.

### 5.3 Empfehlung CosmoRT

**Drei Layer**:

1. **ABI-Layer**: epoll/poll/select voll, Linux-kompatibel. Bleibt fuer
   Alpine/musl/LTP. KEIN Stub, KEINE Vereinfachung.

2. **Kernel-interner Async-Mechanismus**: `wait_queue_entry.func`-Modell
   (siehe §3.3) ist die *Primitive*. epoll ist ein Konsument, io_uring waere
   ein zweiter, audio-pipe waere ein dritter. Alle drei nutzen die selbe
   Waitqueue-Entry-Mechanik.

3. **Phase-19-Audio-API**: io_uring-aehnliche SQ/CQ-Mechanik OHNE Linux-ABI-
   Zwang. Audio-Producer/Consumer mappen ein Ring-Page, kein Syscall im
   Steady State. Siehe `notes/AUDIO.md` fuer Phase-19-Details.

**Nicht** io_uring als Linux-ABI nachbauen — zu viel Surface (200+ Opcodes,
register/unregister-Quirks). epoll fuer ABI, eigener Audio-SQ/CQ fuer Phase 19.

---

## 6. Synchronization Primitives

### 6.1 futex1 vs futex2

| Aspekt | futex1 | futex2 (`futex_waitv`) | Bemerkung |
|--------|--------|------------------------|-----------|
| Wait-on-N | Nein | Ja, bis 128 Eintraege | WaitForMultipleObjects |
| Sizes | u32 only | u8/u16/u32/u64 (Plan, nur u32 final) | NUMA-Hint vorgesehen, nicht final |
| Timeout-Clock | implicit pro Op | `clockid` im Aufruf | sauberer |
| Syscall-Nr (x86_64) | 202 (futex) | 449 (futex_waitv), 454 (futex_wait), 455 (futex_wake), 456 (futex_requeue) | seit 5.16/6.7 |
| ABI-Stabilitaet | stabil | erweitert sich noch | |

Quellen: [docs.kernel.org/userspace-api/futex2.html](https://docs.kernel.org/userspace-api/futex2.html),
[Collabora 2023](https://www.collabora.com/news-and-blog/blog/2023/02/17/the-futex-waitv-syscall-gaming-on-linux/),
[Phoronix: futex_wait/wake/requeue 6.7](https://www.phoronix.com/news/Futex2-Linux-6.7).

### 6.2 Empfehlung CosmoRT

**Ja** zu `futex_waitv`. Begruendung:
- Wine/Proton verwendet es. Sobald wir Doom (Phase 16) wollen — Pflicht.
- pthread_cond_clockwait will eigentlich `futex_waitv` mit clockid.
- Implementierung ist klein: array iterieren, jedes uaddr in Hash-Bucket
  einhaengen, gemeinsamer Wake-Trigger raeumt alle anderen aus, gibt Index zurueck.

**Nein** zu futex_wait/wake/requeue als getrennten Syscalls — wir bauen
intern `do_futex(op, ...)` bereits sauber. Die Splittung ist Linux-
internes Refactoring, nicht ABI-relevant.

**Aktuelles `do_futex`-Cleanup-Liste** (orthogonal zu futex2):
- `event_wait` aus `futex_wait` raus (siehe §4) — `wait_event_interruptible_timeout`
  auf der Bucket-wq.
- `futex_drain_events` ENTFAELLT.
- `EQ_FUTEX_WAKE`/`EQ_TIMEOUT` als event_t-Types ENTFALLEN.
- `FUTEX_WAITER_MAX 256` ist statisches Pool — gegen CLAUDE.md-Regel
  "keine fixen Pools". Slab-allokiert, RLIMIT_NPROC-gebunden.

---

## 7. RT-Patterns (PREEMPT_RT)

### 7.1 Was Linux-RT macht

Quellen: [docs.kernel.org/locking/locktypes.html](https://www.kernel.org/doc/html/latest/locking/locktypes.html),
[rt-mutex-design.html](https://docs.kernel.org/locking/rt-mutex-design.html).

Drei Lock-Typen mit klaren Regeln:

| Typ | Mainline | PREEMPT_RT | Wo benutzen |
|-----|----------|------------|-------------|
| `raw_spinlock_t` | spinlock | spinlock | IRQ-Pfad, Scheduler-Innen, Context-Switch |
| `spinlock_t` | spinlock | sleeping rt_mutex | Allgemeiner Code, normalerweise spin |
| `mutex_t` | sleeping mutex | sleeping rt_mutex | Lange Critical Sections |

PREEMPT_RT konvertiert `spinlock_t` zur Sleeping-Lock mit Priority Inheritance:
ein Low-Prio-Thread, der einen `spinlock_t` haelt und von einem High-Prio-
Thread blockiert wird, erbt dessen Prioritaet bis zum Release. Verhindert
unbounded Priority Inversion.

`raw_spinlock_t` bleibt echter Spinlock — fuer Stellen, wo Sleep verboten
ist (Scheduler-Pick, IRQ-Top-Half).

### 7.2 Empfehlung CosmoRT

**Bereits korrekt**: rq_lock, futex-bucket-lock, eq_lock, wq->lock sind
"raw" Spinlocks (IRQ-disabled). Das ist die richtige Klasse.

**Fehlend**: `mutex_t` mit PI fuer User-Locks (pthread-Mutex jenseits von
futex). Phase 11+ wenn pthread_mutexattr_setprotocol(PTHREAD_PRIO_INHERIT)
echte Tests bekommt.

**Konkretes Audio-Bedarf** (Phase 19):
```
Audio-RT-Thread (Prio 31, SCHED_FIFO):
  futex_wait(audio_period_ready, ...)  — uncontended fast path

Audio-IRQ:
  HDA-IRQ → audio_period_ready++ → futex_wake(1)
  
Maximalblockade darf < 100µs sein.
```

Spinlock-Hold-Zeiten in CosmoRT auditieren (Liste fuer Phase 14):
- Was haelt rq_lock laenger als 1µs? sched_pick: O(1) Bitmap+Liste — OK.
  sched_add: O(1) — OK.
- Was haelt eq_lock laenger als 1µs? eq_grow: pages_alloc — *kann* PMM-Lock
  triggern. **Audit** wert.
- futex-bucket-lock + slab_alloc: gleiche Problemklasse.

**lock_class** als `_Static_assert`:
```c
_Static_assert(sizeof(raw_spinlock_t) == 8, "raw spinlock layout");
```

Wenn wir Phase 14 (Audio) ernst nehmen: Lockdep-aequivalentes Audit-Tooling
(`tools/lock_audit.py` aus Trace-Logs).

---

## 8. Hardware-Features (UINTR, UMWAIT, CAT, …)

### 8.1 UMWAIT/UMONITOR/TPAUSE (WAITPKG)

Quelle: [Intel SDM Vol 2 + LWN 790812](https://lwn.net/Articles/790812/).

Userspace-Instruktionen, kein Syscall. UMONITOR `[addr]` armiert
Cache-Line-Watch, UMWAIT `tsc-deadline` schlaeft bis Store-zu-Line *oder*
Deadline. Maximal-Wait kontrolliert Kernel via `IA32_UMWAIT_CONTROL` (Default
~100µs).

**Use-Case 1: futex fast path ohne Syscall**
```
musl-style spin → UMONITOR(uaddr) → UMWAIT(short) →
  if (*uaddr != val) return  
  → futex(WAIT) syscall fallback
```
Spart Syscall fuer "Wake kommt in <50µs" — relevant fuer locks unter Audio-Prio.

**Use-Case 2: Audio-Sleep fuer kurze Idle-Periods (Phase 19)**
```
Audio-Thread fertig mit Frame, Period-End in 200µs:
  hal_cpu_umwait(now + 200µs)  // statt hlt+IRQ
  → C0.2-State, sub-µs Wakeup
```
HLT-Wakeup auf modernen CPUs ~5-15µs (LAPIC-IRQ-Latenz). UMWAIT ~1-2µs.

**Empfehlung**: Phase 14 (Driver-Audit) detect WAITPKG via CPUID, Phase 19
nutzen. Kein Syscall noetig.

### 8.2 UINTR (User Interrupts)

Quelle: [LWN 869140](https://lwn.net/Articles/869140/),
[NSDI'25](https://www.usenix.org/system/files/nsdi25-guo.pdf).

Sapphire-Rapids+. Hardware-Feature: ein Userspace-Thread schickt SENDUIPI
direkt an einen anderen Userspace-Thread, der per UIRET oder im UPID
Notification-Vektor gewacht/notifiziert wird. Bypassen den Kernel komplett
nach Setup.

Performance:
- 9× schneller als eventfd
- 16× schneller als pipes/signals
- Wakeup-Latenz < 1µs

**Use-Case CosmoRT**: Phase 19 Audio-IPC zwischen Audio-Driver-Userspace-Daemon
und Audio-Apps — ohne Syscall.

**Setup-Komplexitaet**: nicht trivial. UPID-Allocation, IDT-Vektor, MSR-Setup
pro Thread. Kernel-API fehlt in CosmoRT komplett. **Phase 19+**.

### 8.3 CAT (Cache Allocation Technology)

Intel RDT, mainstream seit Skylake-Server. L2/L3-Partition per CBM-Mask via
`IA32_L3_MASK_n` MSRs. Kernel exposed das via `resctrl`-Filesystem.

Audio-Use-Case (Intel-Whitepaper): RT-Audio-Period auf Cache-Mask 0x0F (untere
4 Cache-Ways), Compute-Best-Effort auf 0xF0. Latenz-Reduktion 64% gemessen
fuer PCIe-IRQ-Response.

**Empfehlung**: Phase 19 implementieren als per-process Attribut. Klein
(~150 Zeilen), riesiger Audio-Win.

### 8.4 SHSTK / IBT / LASS

| Feature | Nutzen | Phase |
|---------|--------|-------|
| SHSTK (Shadow Stack, CET) | ROP-Mitigation User+Kernel | optional, Phase 20+ Security |
| IBT (Indirect Branch Tracking) | JOP-Mitigation | dito |
| LASS (Linear Addr Space Sep) | SMAP/SMEP-Erweiterung, CR4.LASS | nice-to-have |
| APIC-Virtualization | irrelevant (kein Hypervisor) | nicht |

CosmoRT-Prio: sind wichtig wenn wir mal Sandbox brauchen. **Nicht** im
kritischen Pfad fuer Phase 10-19.

---

## 9. Konkrete Migrations-Reihenfolge Phase 10.2

**Falsch wäre**: weiter Pfad-fuer-Pfad migrieren mit dem aktuellen
`sched_wake`-Routing-Bug. Jeder weitere migrierte Pfad zerbricht nanosleep01
neu, weil das System-State-Modell inkonsistent bleibt.

**Richtig**:

### Phase 10.2a — Architekturkorrektur (1 Commit, kein Funktions-Refactor)

1. `wait_func_t` und `wait_queue_entry.func` einfuehren.
2. `default_wake_function` / `autoremove_wake_function` schreiben.
3. `try_to_wake_up(t, mask)` einfuehren (CAS state + sched_add).
4. `wake_up(wq, mask, nr)` ruft `e->func(e, mask)` statt eigener
   try_to_wake_up_locked.
5. `sched_wake(t)` wird Wrapper: `try_to_wake_up(t, TASK_NORMAL)`.
6. `t->wait_head` und `t->wait_entry` ENTFERNEN aus `thread_t`.
7. `bind_wait/unbind_wait` in waitqueue.c LOESCHEN.
8. Test: alle 2189 ktest, alle musl/LTP runs identisch zu pre-10.1 baseline.

**Erwartung**: 0 Regressions. Das ist Refactor zu cleaner Wake-Path-Semantik
ohne semantische Aenderung an Sleepers.

### Phase 10.2b — Migrationsreihenfolge (gestaffelt, je 1 Commit)

Niedriges Risiko zuerst (kein Signal/RT-Pfad), dann hoch:

1. **eventfd** — counter+wq, einfach. Risiko: minimal.
2. **pipe** — 2 wqs (read/write). Risiko: minimal.
3. **socket/poll** — viele wqs (sk_wq pro fd). Risiko: mittel.
4. **epoll** — eppoll_entry mit eigenem `func`. Loescht
   `epoll_sleeper_add/remove`, `epoll_wake_all`, per-core sleeper array.
   Risiko: mittel.
5. **futex** — bucket-wq, `wait_event_interruptible_timeout`. Loescht
   `futex_drain_events`, EQ_FUTEX_WAKE-Type. Risiko: hoch (PI-Boost-Pfad
   beruehrt!).
6. **wait4 / process_wait** — proc-wq, `wait_event_interruptible`. Loescht
   `EQ_CHILD_EXITED`-Wakeup-Mechanik. Risiko: hoch (Reaping-Race).
7. **rt_sigtimedwait** — proc->signal_wq + signal_pending(t)-Loop. Loescht
   `event_wait`-Aufruf in signal.c. Risiko: hoch.
8. **event_wait selbst LOESCHEN** — keine Caller mehr. event_queue.c geht
   ersatzlos weg. `thread_t.eq` und `thread_t.eq_lock` entfernen.

**Pro Schritt**:
- Vor dem Commit: `make test-hw` und `make alpine-test` muessen identisch
  zur baseline sein.
- Wenn nicht: NICHT raten. Diff zeigt einen state-Race — finden, fixen, dann commit.

### Phase 10.2c — Aufraeumen (1 Commit)

- `thread_t.eq`, `thread_t.eq_lock` weg.
- `event_queue.c` und `core/event_queue.h` weg.
- `EQ_*`-Konstanten weg (oder, wenn ein Signal-Path noch
  EQ_CHILD_STOPPED braucht: refactor zu proper notification primitive).

**Geschaetzte Diff-Groesse**: -800 / +300 LOC. Nettozugewinn: 500 LOC weg,
zwei Concurrency-Klassen verschwinden.

---

## 10. ABI-kompatibel-aussen vs modern-innen

| Subsystem | Linux-ABI Pflicht | Intern modern moeglich |
|-----------|-------------------|------------------------|
| Syscall-Numbers (x86_64) | Pflicht (musl hardcodes) | n/a |
| errno-Werte | Pflicht | n/a |
| struct stat layout | Pflicht (musl/glibc layout) | n/a |
| signal numbers | Pflicht (POSIX) | n/a |
| futex op codes | Pflicht | Implementation komplett anders OK |
| epoll event flags | Pflicht | Wake-Mechanik intern egal |
| sigaction layout | Pflicht | Delivery-Pfad intern egal |
| **Task State Machine** | **NICHT exposed** | **Kompletter Umbau OK** |
| **Wait Queue Mechanik** | **NICHT exposed** | **Linux-Modell uebernehmen** |
| **Scheduler** | nur SCHED_* Policies + sched_setscheduler | EEVDF/CFS/whatever — frei |
| **Memory mgmt intern** | mmap-ABI Pflicht, Pagealloc intern frei | Buddy/SLUB/eigene |
| **Driver model** | nur via sysfs-Schein | egal |
| **VFS-Implementation** | dirent layout Pflicht | Lookup/cache-Mechanik frei |
| **IPC intern** | shm/sem/msg-IDs Pflicht | Hash/RB/whatever |
| **Audio API (Phase 19)** | KEIN Linux-ABI noetig | komplett eigen, io_uring-style |

**Schluss**: 
- Externe Schicht ist Linux 6.x x86_64 ABI. Punkt.
- Interne Schicht darf alles besser machen, was nicht durch ABI sichtbar ist.

Aktuelle CosmoRT-Verstoesse gegen "nicht exposed":
- `t->wait_head`-Routing ist nicht Linux. Linux hat das nie gehabt. Wir dachten
  wir vereinfachen — wir haben uns etwas erfunden, das schlechter ist.
- `event_queue` als per-thread-Inbox parallel zur waitqueue ist nicht Linux.
  Linux hat *eine* Wake-Primitive (`try_to_wake_up`), die Inbox-Aequivalente
  (signalfd, eventfd, pipe) sind alle ueber Waitqueues gebaut.

---

## Quellen / Code-Referenzen

**Linux Kernel** (alle paths relativ zu torvalds/linux):
- `kernel/sched/core.c::try_to_wake_up`, `signal_wake_up_state`
- `include/linux/sched.h` — TASK_*-Bits, task_struct
- `kernel/signal.c::__send_signal` — Signal-Wake-Sequenz
- `fs/eventpoll.c` — ep_poll_callback, ep->lock rwlock-Conversion
  ([Commit a218cc4914](https://github.com/torvalds/linux/commit/a218cc4914209ac14476cb32769b31a556355b22))
- `kernel/futex/` — futex2-Split (waitv.c, syscalls.c)

**Dokumentation**:
- [docs.kernel.org/locking/rt-mutex-design.html](https://docs.kernel.org/locking/rt-mutex-design.html)
- [docs.kernel.org/locking/locktypes.html](https://www.kernel.org/doc/html/latest/locking/locktypes.html)
- [docs.kernel.org/timers/no_hz.html](https://docs.kernel.org/timers/no_hz.html)
- [docs.kernel.org/userspace-api/futex2.html](https://docs.kernel.org/userspace-api/futex2.html)

**Artikel**:
- [LWN 790812: x86/umwait](https://lwn.net/Articles/790812/)
- [LWN 869140: x86 User Interrupts](https://lwn.net/Articles/869140/)
- [LWN 549580: Nearly full tickless](https://lwn.net/Articles/549580/)
- [LWN 146861: realtime preemption overview](https://lwn.net/Articles/146861/)
- [Collabora: futex_waitv und Gaming](https://www.collabora.com/news-and-blog/blog/2023/02/17/the-futex-waitv-syscall-gaming-on-linux/)
- [danluu: Why Intel added cache partitioning](https://danluu.com/intel-cat/)
- [Intel CAT Whitepaper](https://www.intel.com/content/dam/www/public/us/en/documents/white-papers/cache-allocation-technology-white-paper.pdf)

**Microkernels / Alternativen**:
- [seL4 Reference Manual 14.0.0](https://sel4.systems/Info/Docs/seL4-manual-latest.pdf)
  — Notification + SchedContext-Bind, Priority-ordered IPC
- [Fuchsia Zircon Signals](https://fuchsia.dev/fuchsia-src/concepts/kernel/signals)
  — Port-basierte async wait, Bit-Signals statt Waitqueues
- [NSDI'25 — Understanding Intel User Interrupts](https://www.usenix.org/system/files/nsdi25-guo.pdf)

**CosmoRT-interne Dateien** (zur Orientierung):
- `src/kernel/core/sched.c` — sched_wake (Bug §2.1), schedule, finish_switch
- `src/kernel/core/waitqueue.c` — prepare_to_wait, schedule_timeout
- `src/kernel/core/event_queue.c` — event_post, event_wait (zu loeschen)
- `src/kernel/proc/signal.c` — kill_one (Doppelpfad §4.1)
- `src/kernel/ipc/futex.c` — do_futex, EQ_FUTEX_WAKE-Drain (zu loeschen)
- `src/kernel/event/epoll.c` — epoll_sleeper_add/wake_all (zu loeschen)
- `src/kernel/event/timerfd.c` — als einzige Migration korrekt
- `include/kernel/proc/thread.h:184-185` — wait_entry, wait_head (zu loeschen)
