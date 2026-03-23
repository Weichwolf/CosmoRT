# CosmoRT Adversarial/Crash-Test Plan

Jeder Test folgt dem CRASH_TEST()-Pattern: fork, Child fuehrt Angriff aus,
Parent prueft via wait4 ob Child terminiert (nicht der Kernel).

Systemlimits (relevant fuer Exhaustion-Tests):

| Resource       | Limit            | Define           |
|----------------|------------------|------------------|
| Prozesse       | 16               | PROC_MAX         |
| Threads        | 64               | THREAD_MAX       |
| FDs/Prozess    | 256              | FD_MAX           |
| VMAs global    | 8192             | VMA_MAX          |
| Sockets TCP    | 16               | MAX_SOCKETS      |
| Unix Sockets   | 32               | USOCK_MAX        |
| Futex Waiters  | 256              | FUTEX_WAITER_MAX |
| PID Table      | 256              | PID_TABLE_MAX    |
| TID Table      | 512              | TID_TABLE_MAX    |

---

## 1. Memory Corruption Attacks

### 1.1 Kernel-Adresse schreiben (direkt)

**Datei:** `test/crash/test_kern_write.c`

**Angriff:** Child schreibt direkt auf Kernel-Adressen.

**Schwachstelle:** Page-Table-Isolation. User-PML4 darf keine Kernel-Pages
mit PTE_USER haben. Jeder Zugriff auf >= 0x800000000000 muss GPF (#13)
ausloesen.

**Ablauf:**
1. `volatile uint64_t *p = (volatile uint64_t *)0xFFFF800000000000ULL;`
2. `*p = 0xDEAD;` — muss SIGSEGV ausloesen
3. Wiederhole mit `0xFFFFFFFF80000000` (kanonisch hohe Kernel-Adresse)
4. Wiederhole mit `0x800000000000` (erste Kernel-Adresse)

**Erwartung:** Child bekommt SIGSEGV (Signal 11), wird gekillt. Parent ueberlebt.

**Syscalls:** Keine — reiner Speicherzugriff. SIGSEGV-Handler optional
(Exit-Code pruefen).

**Prioritaet:** KRITISCH — fundamentale Isolation.

---

### 1.2 NULL-Deref

**Datei:** `test/crash/test_null_deref.c`

**Angriff:** Zugriff auf Adresse 0 (und nahe 0: 0x1, 0xFFF).

**Schwachstelle:** NULL-Page darf nicht gemappt sein. `vma_find_free()`
gibt nie Adressen < 0x1000 zurueck, und `mmap` mit Hint 0 waehlt andere
Adresse. Aber MAP_FIXED auf 0x0 muesste abgelehnt werden.

**Ablauf:**
1. `*(volatile int *)0 = 42;` — SIGSEGV
2. `*(volatile int *)0xFFF = 42;` — SIGSEGV
3. `mmap(0, 4096, PROT_RW, MAP_FIXED|MAP_PRIV_ANON, -1, 0)` — muss -EINVAL
   oder -ENOMEM liefern, NICHT erfolgreich mappen
4. Falls mmap erfolgreich: `*(volatile int *)0 = 42;` — darf NICHT
   funktionieren (Kernel darf NULL-Page nie zulassen)

**Erwartung:** Schritte 1-2: SIGSEGV. Schritt 3: Fehler oder Ablehnung.

**Syscalls:** SYS_MMAP (9) mit MAP_FIXED (0x10).

**Prioritaet:** HOCH — NULL-Deref-Exploits basieren auf gemappter NULL-Page.

---

### 1.3 Use-after-munmap

**Datei:** `test/crash/test_use_after_munmap.c`

**Angriff:** Page mappen, schreiben, unmappen, dann lesen/schreiben.

**Schwachstelle:** Nach munmap muss die PTE entfernt sein. TLB-Flush
muss korrekt sein (auf SMP 2 auch via TLB-Shootdown auf dem anderen Core).

**Ablauf:**
1. `addr = mmap(0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0)`
2. `*(volatile int *)addr = 0xBEEF;`
3. `munmap(addr, 4096)` — muss 0 zurueckgeben
4. `*(volatile int *)addr = 0xDEAD;` — muss SIGSEGV ausloesen

**Erwartung:** Schritt 4 loest SIGSEGV aus. TLB enthaelt keinen stale Entry.

**Syscalls:** SYS_MMAP (9), SYS_MUNMAP (11).

**Prioritaet:** HOCH — Use-after-free auf Speicherebene.

---

### 1.4 mprotect auf Kernel-Pages

**Datei:** `test/crash/test_mprotect_kern.c`

**Angriff:** mprotect auf Kernel-Adressen aufrufen.

**Schwachstelle:** `do_mprotect` muss Adressen >= 0x800000000000 ablehnen.

**Ablauf:**
1. `mprotect(0xFFFF800000000000, 4096, PROT_RW)` — muss -ENOMEM oder
   -EINVAL zurueckgeben
2. `mprotect(0x800000000000, 4096, PROT_RW)` — ebenso
3. `mprotect(0x7FFFFFFFE000, 0x10000, PROT_RW)` — Wrap-around ueber
   Kernel-Grenze, muss abgelehnt werden

**Erwartung:** Alle Aufrufe geben negativen Fehlercode zurueck. Kein Kernel-Crash.

**Syscalls:** SYS_MPROTECT (10).

**Prioritaet:** KRITISCH — koennte Kernel-Code beschreibbar machen.

---

### 1.5 SROP: rt_sigreturn mit gefaelschter ucontext (RIP in Kernel)

**Datei:** `test/crash/test_srop.c`

**Angriff:** Gefaelschten Signal-Frame auf den Stack legen, rt_sigreturn
aufrufen mit RIP = Kernel-Adresse.

**Schwachstelle:** `do_rt_sigreturn()` in sys_signal.c restauriert RIP aus
`uc.uc_mcontext.gregs.rip` und RSP aus `uc.uc_mcontext.gregs.rsp` OHNE zu
validieren, dass sie im User-Space liegen. SYSRET setzt RIP = RCX, aber
wenn RCX eine nicht-kanonische oder Kernel-Adresse ist, loest SYSRET
selbst eine GPF aus — im schlimmsten Fall im Kernel-Modus mit User-RSP.

**Ablauf:**
1. `mmap` einen RW-Bereich fuer den gefaelschten Frame
2. Signal-Frame manuell aufbauen:
   - `gregs.rip = 0xFFFFFFFF80000000` (Kernel-Adresse)
   - `gregs.rsp = <gueltiger User-Stack>`
   - `gregs.rax = 42`
   - Alle anderen Register auf 0
3. RSP auf den Frame setzen (inline-asm: `mov %0, %%rsp`)
4. `syscall(SYS_RT_SIGRETURN)` (Nr. 15)

**Erwartung:** Child wird gekillt (GPF oder SIGSEGV). Kernel ueberlebt.
Kernel darf NICHT Code an 0xFFFFFFFF80000000 ausfuehren.

**Varianten:**
- RIP = 0xDEAD000000000000 (nicht-kanonisch)
- RIP = 0x800000000000 (erste Kernel-Adresse)
- RSP = 0xFFFF800000000000 (Kernel-Stack)
- RIP = 0 (NULL)

**Syscalls:** SYS_RT_SIGRETURN (15), SYS_MMAP (9).

**Prioritaet:** KRITISCH — Sigreturn-Oriented Programming. Die fehlende
Validierung in do_rt_sigreturn ist ein realer Bug.

---

### 1.6 Stack-Pivot via sigreturn (RSP in Kernel-Space)

**Datei:** `test/crash/test_stack_pivot.c`

**Angriff:** rt_sigreturn mit RSP = Kernel-Adresse. SYSRET restauriert
User-RSP aus dem Frame. Wenn RSP in Kernel-Space liegt und der Prozess
danach einen Syscall macht, liest/schreibt der Kernel auf seinem
eigenen Stack ueber den User-RSP.

**Schwachstelle:** Gleiche wie 1.5. `cpu->user_rsp = uc.uc_mcontext.gregs.rsp`
ohne Validierung.

**Ablauf:**
1. Gefaelschten Signal-Frame aufbauen:
   - `gregs.rip = <gueltige User-Funktion die sofort exit aufruft>`
   - `gregs.rsp = 0xFFFF800000001000` (Kernel-Stack-Bereich)
2. rt_sigreturn
3. Falls der Prozess ueberlebt: sofort exit_group aufrufen

**Erwartung:** Child crasht oder wird gekillt. Kernel stabil.

**Syscalls:** SYS_RT_SIGRETURN (15).

**Prioritaet:** KRITISCH.

---

### 1.7 mmap(MAP_FIXED) auf existierenden Code

**Datei:** `test/crash/test_mmap_fixed_overwrite.c`

**Angriff:** MAP_FIXED auf die eigene Code-Section, um den laufenden
Code mit beliebigen Daten zu ueberschreiben.

**Schwachstelle:** Nicht direkt ein Kernel-Bug (Userspace darf sich
selbst zerstoeren), aber testet ob der Kernel sauber mit dem resultierenden
Crash umgeht.

**Ablauf:**
1. Eigene RIP-Adresse lesen (inline-asm `lea`)
2. `mmap(rip_page, 4096, PROT_RW, MAP_FIXED|MAP_PRIV_ANON, -1, 0)`
3. Inhalt mit 0xCC (INT3) fuellen
4. Return — springt in INT3-Feld

**Erwartung:** Child bekommt SIGTRAP (5) oder SIGSEGV (11). Parent ueberlebt.

**Syscalls:** SYS_MMAP (9) mit MAP_FIXED.

**Prioritaet:** MITTEL.

---

## 2. Resource Exhaustion

### 2.1 FD-Table voll

**Datei:** `test/crash/test_fd_exhaust.c`

**Angriff:** open()/pipe()/socket() in Schleife bis FD_MAX (256) erreicht.

**Schwachstelle:** fd_alloc muss -EMFILE zurueckgeben wenn FD_MAX erreicht.
Kein OOB-Write in die fd_table.

**Ablauf:**
1. `for (i = 0; i < 300; i++) pipe2(fds, 0)` — jeder pipe-Aufruf
   verbraucht 2 FDs
2. Ab FD 256: muss -EMFILE (24) zurueckgeben
3. Danach: alle FDs schliessen, pruefen ob close funktioniert
4. Neuen FD oeffnen — muss funktionieren (kein Leak)

**Erwartung:** pipe2 gibt -24 zurueck ab FD_MAX. Nach Cleanup funktioniert
alles normal.

**Syscalls:** SYS_PIPE2 (293), SYS_CLOSE (3).

**Prioritaet:** HOCH — FD-Overflow koennte OOB-Schreiben verursachen.

---

### 2.2 Prozess-Table voll (aggressive Fork-Bombe)

**Datei:** `test/crash/test_proc_exhaust.c`

**Angriff:** fork() ohne wait(), bis PROC_MAX (16) erreicht.

**Schwachstelle:** proc_alloc (Slab) muss NULL zurueckgeben, fork muss
-EAGAIN liefern. Children die nicht gereapt werden, muessen als Zombies
korrekt verwaltet werden.

**Ablauf:**
1. `for (i = 0; i < 32; i++) pids[i] = fork()`
   - Children: `nanosleep(1s)` dann exit
2. Irgendwann muss fork -EAGAIN (-11) zurueckgeben
3. Parent: wait4 auf alle PIDs, alle reapen
4. Danach: fork muss wieder funktionieren

**Erwartung:** fork gibt -EAGAIN zurueck. Nach Reaping: System stabil.

**Syscalls:** SYS_FORK (57), SYS_WAIT4 (61), SYS_NANOSLEEP (35),
SYS_EXIT_GROUP (231).

**Prioritaet:** HOCH — Zombie-Leak fuehrt zu permanentem DoS.

---

### 2.3 VMA-Exhaustion

**Datei:** `test/crash/test_vma_exhaust.c`

**Angriff:** Tausende kleine mmaps um den VMA-Slab (8192 Eintraege) zu
erschoepfen.

**Schwachstelle:** vma_alloc_raw() gibt NULL zurueck bei Erschoepfung.
mmap muss -ENOMEM liefern, nicht crashen. VMA-Slab ist global —
ein Prozess kann VMAs fuer andere Prozesse blockieren.

**Ablauf:**
1. `for (i = 0; i < 9000; i++) mmap(0, 4096, PROT_NONE, MAP_PRIV_ANON, -1, 0)`
2. Ab ~8192: muss -ENOMEM zurueckgeben
3. mprotect auf eine existierende Mapping — darf nicht durch VMA-Split
   fehlschlagen und inkonsistenten State hinterlassen
4. Cleanup: munmap aller Adressen

**Erwartung:** -ENOMEM nach VMA-Limit. Kein Kernel-Crash. System erholt sich.

**Syscalls:** SYS_MMAP (9), SYS_MUNMAP (11), SYS_MPROTECT (10).

**Prioritaet:** HOCH — globaler Slab, Prozess-uebergreifende Auswirkung.

---

### 2.4 Futex-Waiter-Pool voll

**Datei:** `test/crash/test_futex_exhaust.c`

**Angriff:** FUTEX_WAITER_MAX (256) Threads die alle auf verschiedenen
Futexen blockieren.

**Schwachstelle:** waiter_slab erschoepft. futex_wait muss -ENOMEM
zurueckgeben, nicht crashen.

**Ablauf:**
1. Mehrere Children forken (PROC_MAX erlaubt ~14 nach Parent + ktest)
2. Jedes Child: mmap eine Page, `*(uint32_t*)addr = 1`,
   `futex(addr, FUTEX_WAIT, 1, NULL, NULL, 0)`
3. Wenn waiter_slab voll: futex_wait gibt -ENOMEM zurueck
4. Parent: alle Children killen (SYS_KILL), wait4

**Erwartung:** -ENOMEM statt Crash. Children aufgeraeumt.

**Syscalls:** SYS_FUTEX (202), SYS_FORK (57), SYS_KILL (62), SYS_WAIT4 (61).

**Prioritaet:** MITTEL — 256 Waiters ist ausreichend fuer reale Workloads,
aber Exhaustion-Resilienz muss gewaehrleistet sein.

---

### 2.5 Socket-Pool voll

**Datei:** `test/crash/test_socket_exhaust.c`

**Angriff:** socket() in Schleife bis MAX_SOCKETS (16) und USOCK_MAX (32)
erschoepft.

**Schwachstelle:** sock_alloc und usock_alloc muessen NULL/0 zurueckgeben.
socket() muss -EMFILE liefern.

**Ablauf:**
1. `for (i = 0; i < 40; i++) socket(AF_INET, SOCK_STREAM, 0)` — TCP
2. Ab 16: muss -EMFILE zurueckgeben
3. `for (i = 0; i < 40; i++) socket(AF_UNIX, SOCK_STREAM, 0)` — Unix
4. Ab 32: muss -EMFILE zurueckgeben
5. Cleanup: close alle FDs

**Erwartung:** Saubere Fehler-Codes, kein OOB.

**Syscalls:** SYS_SOCKET (41), SYS_CLOSE (3).

**Prioritaet:** MITTEL.

---

### 2.6 Pipe-Buffer voll + Deadlock

**Datei:** `test/crash/test_pipe_full.c`

**Angriff:** Pipe erstellen, Schreib-Ende mit grossen Writes fuellen
bis der Buffer voll ist. Dann: gleicher Thread liest UND schreibt
(Deadlock-Versuch).

**Schwachstelle:** Pipe-Write muss -EAGAIN oder blockieren (mit Timeout).
Kein Kernel-Deadlock wenn ein Thread auf eigener Pipe blockiert.

**Ablauf:**
1. `pipe2(fds, O_NONBLOCK)`
2. `while (write(fds[1], buf, 4096) > 0) count++;`
3. Pruefen: write gibt irgendwann -EAGAIN (-11) zurueck
4. read(fds[0]): muss die geschriebenen Daten liefern
5. Ohne O_NONBLOCK: write in Schleife in Child, Parent liest nicht —
   Child muss blockieren, nicht crashen. Parent killt Child nach Timeout.

**Erwartung:** Kein Deadlock, kein Kernel-Hang. -EAGAIN bei non-blocking.

**Syscalls:** SYS_PIPE2 (293), SYS_WRITE (1), SYS_READ (0).

**Prioritaet:** HOCH — Pipe-Deadlock auf Single-Thread ist realistisches
Szenario.

---

### 2.7 epoll-Stress

**Datei:** `test/crash/test_epoll_exhaust.c`

**Angriff:** epoll_create1 in Schleife, dann epoll_ctl mit tausenden
von FDs.

**Schwachstelle:** Epoll-Slab-Exhaustion. epoll_ctl muss saubere
Fehler zurueckgeben.

**Ablauf:**
1. `for (i = 0; i < 300; i++) epoll_create1(0)` — verbraucht FDs
2. Pipe erstellen, `epoll_ctl(ADD)` auf read-Ende wiederholt
   (verschiedene epoll-Instanzen)
3. `epoll_wait` mit timeout=0 auf leeren epoll — muss 0 zurueckgeben
4. `epoll_ctl(ADD)` mit ungueltigem FD — muss -EBADF zurueckgeben

**Erwartung:** Saubere Fehler-Codes ab FD-Limit.

**Syscalls:** SYS_EPOLL_CREATE1 (291), SYS_EPOLL_CTL (233),
SYS_EPOLL_WAIT (232), SYS_PIPE2 (293).

**Prioritaet:** MITTEL.

---

## 3. Race Conditions (SMP 2)

Hinweis: SMP 2 bedeutet RT-Core (Core 0, alle IRQs) und Compute-Core
(Core 1, Userspace). Races zwischen Syscalls auf Compute-Core und
IRQ-Handlern auf RT-Core sind das primaere Angriffsmodell. Fork erzeugt
einen zweiten Prozess der auf dem gleichen Compute-Core laeuft (preemptives
Scheduling), nicht echte Parallelitaet auf 2 Compute-Cores.

### 3.1 Concurrent fork+exit

**Datei:** `test/crash/test_race_fork_exit.c`

**Angriff:** Schnelle fork/exit-Zyklen: Parent forkt, Child exittet
sofort, Parent forkt wieder bevor wait4.

**Schwachstelle:** proc_pool und pid_table Races. Zombie-State
muss konsistent sein. Doppeltes Free bei gleichzeitigem exit und
wait4 auf gleichen PID.

**Ablauf:**
1. `for (i = 0; i < 200; i++)`:
   a. `pid = fork()`, Child: `exit_group(0)` sofort
   b. Parent: KEIN wait4
2. Nach der Schleife: `while (wait4(-1, &status, WNOHANG, 0) > 0) reaped++;`
3. Pruefen: reaped <= 200, kein Kernel-Crash

**Erwartung:** Zombies akkumulieren bis PROC_MAX, fork gibt -EAGAIN.
Reaping raeumt auf. Kein Use-after-free in proc_pool.

**Syscalls:** SYS_FORK (57), SYS_EXIT_GROUP (231), SYS_WAIT4 (61)
mit WNOHANG (1).

**Prioritaet:** HOCH — fork/exit ist der meistgenutzte Pfad.

---

### 3.2 Concurrent mmap+munmap auf gleicher Region

**Datei:** `test/crash/test_race_mmap_munmap.c`

**Angriff:** Zwei Children: einer mappt wiederholt MAP_FIXED auf eine
Adresse, der andere unmappt sie.

**Schwachstelle:** VMA-Tree (AVL) und Page-Table-Updates sind nicht
atomar. Auf SMP 2 mit einem Compute-Core und preemptivem Scheduler
sind echte Races moeglich wenn der Timer-IRQ zwischen VMA-Lookup und
PTE-Write preemptet.

**Ablauf:**
1. Parent mappt `addr = mmap(0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0)`
2. Fork 2 Children:
   - Child A: `for (i = 0; i < 1000; i++) mmap(addr, 4096, PROT_RW, MAP_FIXED|MAP_PRIV_ANON, -1, 0)`
   - Child B: `for (i = 0; i < 1000; i++) munmap(addr, 4096)`
3. Beide Children: exit_group(0)
4. Parent: wait4 auf beide

Hinweis: Da fork Address-Spaces kopiert, operieren A und B auf
VERSCHIEDENEN Page-Tables. Fuer echte Races auf dem gleichen
Address-Space braeuchte man clone() mit CLONE_VM.

**Alternative mit clone (falls verfuegbar):**
1. clone(CLONE_VM|CLONE_SIGHAND, ...) — shared Address Space
2. Thread A: mmap-Schleife
3. Thread B: munmap-Schleife
4. Pruefe: kein Kernel-Panic, VMAs konsistent

**Erwartung:** Keine Kernel-Panic. Fehlerhafte Operationen geben -ENOMEM
oder -EINVAL zurueck.

**Syscalls:** SYS_MMAP (9), SYS_MUNMAP (11), SYS_CLONE (56) oder SYS_FORK (57).

**Prioritaet:** HOCH — VMA-Corruption koennte zu Kernel-Panic fuehren.

---

### 3.3 Concurrent close+read auf gleichem FD

**Datei:** `test/crash/test_race_close_read.c`

**Angriff:** Pipe erstellen. clone(CLONE_FILES) fuer shared FD-Table.
Thread A: read auf read-Ende. Thread B: close auf read-Ende.

**Schwachstelle:** fd_get() gibt Pointer auf fd_entry zurueck. Wenn
fd_close() den Entry auf FD_NONE setzt waehrend read() den Pointer
benutzt, entsteht ein Data-Race auf fd_entry_t.

**Ablauf:**
1. `pipe2(fds, O_NONBLOCK)`
2. `clone(CLONE_VM|CLONE_FILES|CLONE_SIGHAND, stack, ...)`
   - Thread: `close(fds[0])` in Schleife (ignoriert EBADF)
3. Parent: `read(fds[0], buf, 64)` in Schleife
4. Pruefe: kein Crash, letzter read gibt -EBADF

**Erwartung:** Race-Safe. Kein Use-after-free in FD-Table.

**Syscalls:** SYS_CLONE (56), SYS_PIPE2 (293), SYS_READ (0), SYS_CLOSE (3).

**Prioritaet:** HOCH — FD-Races sind klassische Kernel-Bugs.

---

### 3.4 Signal waehrend fork

**Datei:** `test/crash/test_race_signal_fork.c`

**Angriff:** SIGALRM-Handler installieren. alarm(0) (sofort).
Waehrend fork den Address-Space kopiert, wird der Signal-Handler
aufgerufen.

**Schwachstelle:** fork ist nicht atomar. copy_address_space() iteriert
VMAs und kopiert Pages. Ein Signal kann den Copy unterbrechen, was zu
inkonsistentem State fuehrt wenn der Signal-Handler neue VMAs erstellt.

**Ablauf:**
1. Signal-Handler fuer SIGUSR1 installieren (Handler: `got_signal = 1; return`)
2. Child A forken: sendet `kill(getppid(), SIGUSR1)` in Schleife
3. Parent: `for (i = 0; i < 50; i++) { pid = fork(); if (pid==0) exit(0); wait4(pid); }`
4. Child A killen, wait4

**Erwartung:** Alle fork/exit-Zyklen erfolgreich. Signal-Handler wird
aufgerufen ohne Korruption.

**Syscalls:** SYS_RT_SIGACTION (13), SYS_KILL (62), SYS_FORK (57),
SYS_WAIT4 (61).

**Prioritaet:** MITTEL — Realistische Interaktion, aber Single-Compute-Core
reduziert echte Parallelitaet.

---

### 3.5 exit waehrend Syscall (clone + exit_group)

**Datei:** `test/crash/test_race_exit_syscall.c`

**Angriff:** clone(CLONE_VM) einen Thread. Haupt-Thread ruft
exit_group() auf waehrend der Clone-Thread in einem blockierenden
Syscall ist (nanosleep, futex_wait, poll).

**Schwachstelle:** exit_kill_process() setzt `scan->state = THREAD_DEAD`
fuer andere Threads. Wenn der Thread gerade in futex_wait blockiert
und ein Waiter-Entry hat, wird der nie freigegeben — Waiter-Slab-Leak.

**Ablauf:**
1. `clone(CLONE_VM|CLONE_SIGHAND, stack, ...)`
   - Thread: `nanosleep(10s)` — blockiert
2. Parent: wartet 10ms, dann `exit_group(0)`
3. Wrapper-Prozess (Grosseltern): wait4 prueft ob Child-Gruppe terminiert

**Erwartung:** Kernel raeumt beide Threads auf. Kein Waiter-Leak,
kein Kernel-Panic.

**Syscalls:** SYS_CLONE (56), SYS_NANOSLEEP (35), SYS_EXIT_GROUP (231).

**Prioritaet:** HOCH — exit_group waehrend Blocking ist fehleranfaellig.

---

## 4. Privilege Escalation

### 4.1 CosmoRT HW-Primitives ohne Driver-Flag

**Datei:** `test/crash/test_hw_privesc.c`

**Angriff:** Normaler Prozess (is_driver=0) ruft SYS_COSMO_* Syscalls auf.

**Schwachstelle:** HW_CAP_CHECK() in dispatch.c prueft `p->is_driver`.
Ein Normalprozess darf keinen Zugriff auf MMIO, DMA, IRQ, PCI haben.

**Ablauf:**
1. `syscall(512, phys, size, &virt)` — SYS_COSMO_MMIO_MAP: muss -EPERM (-1)
2. `syscall(513, size, &virt, &phys)` — SYS_COSMO_DMA_ALLOC: muss -EPERM
3. `syscall(515, irq, handler, arg)` — SYS_COSMO_IRQ_REGISTER: muss -EPERM
4. `syscall(516, bus, dev, func, reg, &val)` — SYS_COSMO_PCI_READ: muss -EPERM
5. `syscall(520, buf, len)` — SYS_COSMO_KEXEC: muss -EPERM

**Erwartung:** Alle geben -EPERM zurueck.

**Syscalls:** SYS_COSMO_MMIO_MAP (512), SYS_COSMO_DMA_ALLOC (513),
SYS_COSMO_IRQ_REGISTER (515), SYS_COSMO_PCI_READ (516),
SYS_COSMO_KEXEC (520).

**Prioritaet:** KRITISCH — DMA und kexec ermoeglichen vollstaendige
Kernel-Kontrolle.

---

### 4.2 is_driver-Flag faelschen via fork von init

**Datei:** `test/crash/test_driver_flag_fork.c`

**Angriff:** init (PID 1) hat `is_driver=1`. Wenn ktest als Kind von
init laeuft, pruefe ob das Kind is_driver erbt.

**Schwachstelle:** do_fork() kopiert den process_t. Wenn is_driver
mitkopiert wird, erbt jeder Kindprozess von init Driver-Rechte.

**Ablauf:**
1. `pid = fork()` (aus ktest, das Kind von init ist)
2. Child: `syscall(512, 0xFEE00000, 4096, &virt)` — SYS_COSMO_MMIO_MAP
3. Pruefen: muss -EPERM zurueckgeben (Kind darf NICHT is_driver erben)

**Erwartung:** -EPERM. Driver-Flag wird nicht vererbt.

**Hinweis:** Falls der Test zeigt, dass is_driver vererbt wird, ist das
ein SEC-CRIT Bug.

**Syscalls:** SYS_FORK (57), SYS_COSMO_MMIO_MAP (512).

**Prioritaet:** KRITISCH.

---

### 4.3 ioctl auf beliebige FD-Typen

**Datei:** `test/crash/test_ioctl_fuzz.c`

**Angriff:** ioctl mit verschiedenen Requests auf verschiedene FD-Typen
(serial, pipe, socket, epoll, timerfd, eventfd).

**Schwachstelle:** do_ioctl muss jeden FD-Typ und Request korrekt
ablehnen oder behandeln. Unbekannte Requests muessen -ENOTTY zurueckgeben.

**Ablauf:**
1. Fuer jeden FD-Typ einen FD oeffnen
2. `ioctl(fd, 0xDEADBEEF, 0)` — unbekannter Request
3. `ioctl(fd, TCGETS, kernel_addr)` — PTY-Request auf Nicht-PTY
4. `ioctl(fd, TIOCGWINSZ, 0)` — NULL-Pointer
5. `ioctl(fd, FIONREAD, kernel_addr)` — Kernel-Adresse als Buffer

**Erwartung:** -ENOTTY oder -EFAULT. Kein Crash.

**Syscalls:** SYS_IOCTL (16), diverse FD-Erstellungs-Syscalls.

**Prioritaet:** MITTEL.

---

## 5. Kernel-State-Corruption

### 5.1 rt_sigreturn ohne vorherige Signalauslieferung

**Datei:** `test/crash/test_sigreturn_unprompted.c`

**Angriff:** rt_sigreturn (Nr. 15) aufrufen ohne dass ein Signal
ausgeliefert wurde. Stack enthaelt keinen gueltigen Signal-Frame.

**Schwachstelle:** do_rt_sigreturn() liest ucontext von `user_rsp - 8 + offset`.
Wenn kein Signal-Frame dort liegt, liest es Garbage-Daten und restauriert
sie als Register. RIP und RSP werden unvalidiert aus Garbage geladen.

**Ablauf:**
1. Direkt `syscall(15)` aufrufen (SYS_RT_SIGRETURN)
2. Stack enthaelt keinen gültigen Frame — RIP/RSP werden Muell

**Erwartung:** Child crasht (SIGSEGV wegen ungueltigem RIP). Kernel
ueberlebt. KEIN RIP in Kernel-Space.

**Syscalls:** SYS_RT_SIGRETURN (15).

**Prioritaet:** KRITISCH — ungepruefter sigreturn ist ein klassischer
Exploitation-Vektor.

---

### 5.2 clone mit widersprüchlichen Flags

**Datei:** `test/crash/test_clone_bad_flags.c`

**Angriff:** clone() mit absurden Flag-Kombinationen.

**Schwachstelle:** do_clone muss inkonsistente Flags ablehnen statt
Undefined Behavior.

**Ablauf:**
1. `clone(CLONE_VM, NULL, NULL, NULL, 0)` — kein Stack, CLONE_VM
2. `clone(CLONE_THREAD, NULL, NULL, NULL, 0)` — CLONE_THREAD ohne CLONE_SIGHAND
3. `clone(0xFFFFFFFF, NULL, NULL, NULL, 0)` — alle Flags gesetzt
4. `clone(CLONE_NEWNS|CLONE_NEWPID, NULL, NULL, NULL, 0)` — Namespace-Flags
   die nicht implementiert sind
5. `clone3({.flags = ~0ULL}, sizeof(clone_args))` — alle Bits gesetzt

**Erwartung:** -EINVAL fuer jede ungueltige Kombination.

**Syscalls:** SYS_CLONE (56), SYS_CLONE3 (435).

**Prioritaet:** HOCH — clone ist der komplexeste Syscall.

---

### 5.3 execve mit manipuliertem ELF

**Datei:** `test/crash/test_execve_bad_elf.c`

**Angriff:** ELF-Datei auf CosmoFS/ramfs schreiben mit:
- Overlapping PT_LOAD Segmente
- p_memsz = 0xFFFFFFFFFFFFFFFF (riesig)
- p_vaddr in Kernel-Space (>= 0x800000000000)
- Mehr als 100 PT_LOAD Segmente
- Ungueltiger e_entry (Kernel-Adresse)

**Schwachstelle:** elf_load muss alle Felder validieren. map_user_page
lehnt vaddr >= 0x800000000000 ab, aber p_memsz-Overflow koennte
die Validierung umgehen.

**Ablauf:**
1. Minimales ELF mit ELFMAG, korrekte e_phoff
2. PT_LOAD mit p_vaddr = 0xFFFF800000000000 — muss abgelehnt werden
3. PT_LOAD mit p_memsz = 0x7FFFFFFFFFFFFFF0 — Integer-Overflow bei
   seg_end-Berechnung
4. `execve("/tmp/bad_elf", NULL, NULL)`
5. Pruefen: execve gibt -EINVAL oder -ENOEXEC zurueck

**Erwartung:** Kein Kernel-Crash. execve schlaegt fehl.

**Syscalls:** SYS_EXECVE (59), SYS_OPEN (2), SYS_WRITE (1) (zum Erzeugen).

**Prioritaet:** HOCH — ELF-Parser ist klassische Angriffsflaeche.

---

### 5.4 Symlink-Loop (ELOOP)

**Datei:** `test/crash/test_symlink_loop.c`

**Angriff:** Zirkulaere Symlinks erstellen: A -> B, B -> A.
Dann open("A").

**Schwachstelle:** VFS-Pfadaufloesung muss Symlink-Tiefe begrenzen
(ELOOP bei > ~40 Stufen). Ohne Limit: Stack-Overflow im Kernel.

**Ablauf:**
1. `symlink("B", "/tmp/A")` (SYS_SYMLINK = 88)
2. `symlink("A", "/tmp/B")`
3. `open("/tmp/A", O_RDONLY)` — muss -ELOOP (-40) zurueckgeben
4. `stat("/tmp/A")` — ebenso -ELOOP

**Erwartung:** -ELOOP. Keine endlose Rekursion im Kernel.

**Syscalls:** SYS_SYMLINK (88), SYS_OPEN (2), SYS_STAT (4).

**Prioritaet:** HOCH — Kernel-Stack-Overflow bei unbegrenzter Rekursion.

---

### 5.5 Path-Traversal

**Datei:** `test/crash/test_path_traversal.c`

**Angriff:** Pfade mit `/../../../..` um aus dem Dateisystem auszubrechen.

**Schwachstelle:** VFS muss `..` ueber Root hinaus ignorieren.
`/../../etc/passwd` muss zu `/etc/passwd` aufgeloest werden.

**Ablauf:**
1. `open("/../../../proc/self/exe", O_RDONLY)` — muss wie `/proc/self/exe` funktionieren
2. `open("/tmp/../../../tmp/test", O_RDONLY)` — muss `/tmp/test` sein
3. `openat(dirfd, "../../../../etc/passwd", O_RDONLY)` — darf nicht
   ueber Root hinausgehen

**Erwartung:** Korrekte Pfadaufloesung. Kein Zugriff ausserhalb des Dateisystems.

**Syscalls:** SYS_OPEN (2), SYS_OPENAT (257).

**Prioritaet:** MITTEL.

---

### 5.6 Pfade > PATH_MAX

**Datei:** `test/crash/test_long_path.c`

**Angriff:** Extrem lange Pfade an Syscalls uebergeben.

**Schwachstelle:** copy_path_from_user() hat max-Parameter. Aber:
wird der Pfad immer ueber copy_path_from_user kopiert, oder gibt
es Code-Pfade die direkt auf den User-Pointer zugreifen?

**Ablauf:**
1. 4096-Byte-Pfad (alles '/') auf User-Stack
2. `open(long_path, O_RDONLY)` — muss -ENAMETOOLONG (-36) zurueckgeben
3. `mkdir(long_path, 0755)` — ebenso
4. `chdir(long_path)` — ebenso
5. 0-Byte-Pfad: `open("", O_RDONLY)` — muss -ENOENT zurueckgeben

**Erwartung:** -ENAMETOOLONG fuer alle. Kein Buffer-Overflow.

**Syscalls:** SYS_OPEN (2), SYS_MKDIR (83), SYS_CHDIR (80).

**Prioritaet:** HOCH — Buffer-Overflow in Pfadverarbeitung.

---

### 5.7 fcntl/dup Manipulation

**Datei:** `test/crash/test_fcntl_abuse.c`

**Angriff:** fcntl und dup mit Grenzwerten.

**Schwachstelle:** dup/dup2 auf Out-of-Range FDs (negativ, >= FD_MAX).

**Ablauf:**
1. `dup(-1)` — -EBADF
2. `dup(FD_MAX)` — -EBADF
3. `dup2(0, -1)` — -EBADF
4. `dup2(0, FD_MAX)` — -EBADF oder -EINVAL
5. `fcntl(0, -1, 0)` — ungueltiger Command, muss -EINVAL zurueckgeben
6. `fcntl(-1, F_GETFL, 0)` — -EBADF
7. `dup2(0, 0)` — muss 0 zurueckgeben (no-op per POSIX)

**Erwartung:** Korrekte Fehlercodes. Kein OOB-Zugriff.

**Syscalls:** SYS_DUP (32), SYS_DUP2 (33), SYS_FCNTL (72).

**Prioritaet:** MITTEL.

---

## 6. Denial of Service

### 6.1 Busywait (Scheduler-Preemption)

**Datei:** `test/crash/test_busywait.c`

**Angriff:** Child fuehrt `while(1){}` aus. Parent muss trotzdem
nach Timeout den Child killen koennen.

**Schwachstelle:** Timer-IRQ (Vector 32) auf RT-Core muss
sched_preempt() triggern, das den Compute-Core-Thread preemptet.
Ohne Preemption haengt der Compute-Core.

**Ablauf:**
1. `pid = fork()`
2. Child: `while(1) { asm volatile(""); }` — Endlosschleife
3. Parent: `nanosleep(100ms)`, dann `kill(pid, SIGKILL)`, wait4
4. Pruefen: wait4 kehrt zurueck, Status = 9 (SIGKILL)

**Erwartung:** Parent ueberlebt, Child wird gekillt. Scheduler preemptet.

**Syscalls:** SYS_FORK (57), SYS_NANOSLEEP (35), SYS_KILL (62),
SYS_WAIT4 (61).

**Prioritaet:** KRITISCH — grundlegende Scheduler-Korrektheit.

---

### 6.2 Signalstorm

**Datei:** `test/crash/test_signal_storm.c`

**Angriff:** kill() in Schleife auf eigenen Prozess oder Child.
Tausende Signale pro Sekunde.

**Schwachstelle:** sig_pending ist ein Bitmask (uint64_t). Gleiche
Signale werden gemerged. Aber: die Signal-Delivery-Schleife in
check_pending_signals muss terminieren. Wenn der Handler selbst
ein Signal sendet (re-entrancy), koennte eine Endlosschleife entstehen.

**Ablauf:**
1. Handler fuer SIGUSR1 installieren: `sig_count++; return;`
2. `for (i = 0; i < 10000; i++) kill(getpid(), SIGUSR1)`
3. Pruefen: sig_count >= 1 (Bitmask merged, exakte Zahl egal)
4. Prozess terminiert normal

**Variante:**
1. Child forken
2. Parent: `for (i = 0; i < 10000; i++) kill(child_pid, SIGUSR1)`
3. Child: nanosleep(1s), exit
4. Pruefen: Child terminiert, kein Kernel-Hang

**Erwartung:** Kein Kernel-Hang, kein Stack-Overflow in Signal-Delivery.

**Syscalls:** SYS_KILL (62), SYS_RT_SIGACTION (13).

**Prioritaet:** MITTEL.

---

### 6.3 Endlos-Syscall (nanosleep mit MAX)

**Datei:** `test/crash/test_infinite_sleep.c`

**Angriff:** nanosleep mit tv_sec = 0x7FFFFFFFFFFFFFFF (MAX_LONG).

**Schwachstelle:** Timer-Berechnung koennte ueberlaufen. deadline =
timer_ms() + sec*1000 + nsec/1000000 muss vor Overflow schuetzen.

**Ablauf:**
1. `pid = fork()`
2. Child: `struct timespec ts = {0x7FFFFFFFFFFFFFFF, 0}; nanosleep(&ts, NULL);`
3. Parent: `nanosleep(100ms)`, `kill(pid, SIGKILL)`, wait4
4. Pruefen: Child wird gekillt, kein Kernel-Hang

**Erwartung:** Child blockiert, wird sauber gekillt.

**Syscalls:** SYS_NANOSLEEP (35), SYS_KILL (62).

**Prioritaet:** MITTEL — Timer-Overflow koennte zu Sofort-Wakeup oder
Overflow-Crash fuehren.

---

### 6.4 Ungueltige Syscall-Nummern

**Datei:** `test/crash/test_bad_syscall.c`

**Angriff:** Syscalls mit ungueltigen Nummern.

**Schwachstelle:** Default-Case in sys_dispatch muss -ENOSYS zurueckgeben.
Negative Nummern, sehr grosse Nummern.

**Ablauf:**
1. `syscall(-1, 0,0,0,0,0)` — muss -ENOSYS (-38)
2. `syscall(9999, 0,0,0,0,0)` — ebenso
3. `syscall(0x7FFFFFFF, 0,0,0,0,0)` — ebenso
4. `syscall(512, 0,0,0,0,0)` — SYS_COSMO_MMIO_MAP als Nicht-Driver: -EPERM
5. Fuer jeden bekannten Syscall: mit offensichtlich falschen Argumenten

**Erwartung:** -ENOSYS oder spezifischer Fehler. Nie Crash.

**Syscalls:** Alle moeglichen Nummern.

**Prioritaet:** HOCH — Defense in Depth.

---

## 7. Zusammenfassung und Priorisierung

| Prio     | Test                          | Wahrscheinlichkeit Bug |
|----------|-------------------------------|------------------------|
| KRITISCH | 1.5 SROP (sigreturn RIP)      | SEHR HOCH — Validierung fehlt |
| KRITISCH | 1.6 Stack-Pivot (sigreturn RSP)| SEHR HOCH — gleicher Bug |
| KRITISCH | 5.1 sigreturn ohne Signal     | SEHR HOCH — gleicher Bug |
| KRITISCH | 4.1 HW-Primitives ohne Driver | HOCH — Fehlconfig bricht Isolation |
| KRITISCH | 4.2 is_driver-Fork            | HOCH — Vererbung unklar |
| KRITISCH | 1.1 Kernel-Adresse schreiben  | NIEDRIG (PTE sollte schuetzen) |
| KRITISCH | 1.4 mprotect auf Kernel       | NIEDRIG (Adress-Check existiert) |
| KRITISCH | 6.1 Busywait/Preemption       | MITTEL |
| HOCH     | 5.3 Bad-ELF execve            | HOCH — Parser-Bugs |
| HOCH     | 5.4 Symlink-Loop              | HOCH — Rekursion unklar |
| HOCH     | 5.6 Pfade > PATH_MAX          | MITTEL |
| HOCH     | 2.1 FD-Exhaustion             | MITTEL |
| HOCH     | 2.2 Proc-Exhaustion           | MITTEL |
| HOCH     | 2.3 VMA-Exhaustion            | MITTEL |
| HOCH     | 2.6 Pipe-Buffer-Deadlock      | MITTEL |
| HOCH     | 3.1 fork+exit Race            | MITTEL |
| HOCH     | 3.2 mmap+munmap Race          | MITTEL |
| HOCH     | 3.3 close+read Race           | MITTEL |
| HOCH     | 3.5 exit waehrend Syscall     | MITTEL |
| HOCH     | 5.2 clone bad flags           | MITTEL |
| HOCH     | 6.4 Bad Syscall-Nummern       | NIEDRIG |
| MITTEL   | 1.2 NULL-Deref + MAP_FIXED    | NIEDRIG |
| MITTEL   | 1.3 Use-after-munmap          | NIEDRIG |
| MITTEL   | 1.7 MAP_FIXED auf Code        | NIEDRIG |
| MITTEL   | 2.4 Futex-Exhaustion          | NIEDRIG |
| MITTEL   | 2.5 Socket-Exhaustion         | NIEDRIG |
| MITTEL   | 2.7 epoll-Stress              | NIEDRIG |
| MITTEL   | 3.4 Signal waehrend fork      | NIEDRIG |
| MITTEL   | 4.3 ioctl-Fuzz                | NIEDRIG |
| MITTEL   | 5.5 Path-Traversal            | NIEDRIG |
| MITTEL   | 5.7 fcntl/dup Abuse           | NIEDRIG |
| MITTEL   | 6.2 Signalstorm               | NIEDRIG |
| MITTEL   | 6.3 Infinite nanosleep        | NIEDRIG |

## 8. Implementation-Hinweise

Alle Tests in `test/crash/`, ein Test pro Datei. Includes:

```c
#include "ktest.h"
```

Pattern fuer jeden Test:

```c
static void test_name(void) {
    puts("\n[Testname]\n");
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* === ANGRIFF === */
        sc1(SYS_EXIT_GROUP, 0);      /* falls ueberlebt */
        __builtin_unreachable();
    }
    check("fork", pid > 0);
    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child terminated", status != 0 || ...);
}
CRASH_TEST("crash/name", test_name);
```

Fuer Race-Condition-Tests die clone brauchen: Stack via mmap allokieren:

```c
long stack = sc6(SYS_MMAP, 0, 65536, PROT_RW, MAP_PRIV_ANON, -1, 0);
long tid = sc5(SYS_CLONE, CLONE_VM|CLONE_SIGHAND, stack + 65536, 0, 0, 0);
```

Fuer sigreturn-Tests: Signal-Frame manuell auf Stack legen, RSP setzen:

```c
/* Frame auf gemappte Page legen */
long frame = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
/* ucontext bei frame + SIGFRAME_OFF_UCONTEXT aufbauen */
/* RSP auf frame setzen, dann syscall(15) */
```
