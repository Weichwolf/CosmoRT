# CosmoRT ABI Conformance Testsuite

Testplan fuer ABI-Invarianten unter Stress. Ziel: Bugs finden, die funktionale
Tests nicht abdecken — Register-Korruption, Alignment-Verletzungen,
Timing-Races, fehlende State-Isolation.

Prioritaet: P0 (hoechste Bugwahrscheinlichkeit) bis P2 (wuenschenswert).

Referenz-Implementation: `test/unit/test_signal_bugs.c` (Vorlage fuer Stil,
Macros, Inline-Assembly).

## Konventionen

```c
#include "ktest.h"
// sc0..sc6 aus test/syscall.h (Inline-Syscall)
// check(), check_val(), check_ge() aus test/ktest.h
// TEST("name", fn) registriert Test in .ktest Section
```

Datei: `test/unit/test_abi_conformance.c` (alle Tests in einer Datei,
thematisch gruppiert durch Kommentar-Banner).

---

## 1. Register-Erhaltung ueber Syscalls (P0)

### ABI-Invariante

x86_64 System V ABI + Linux SYSCALL-Konvention: RBX, RBP, R12-R15 sind
callee-saved und muessen ueber SYSCALL erhalten bleiben. RSP wird ueber
`gs:8` (percpu user_rsp) gesichert/wiederhergestellt. RCX und R11 werden
durch SYSCALL/SYSRET zerstoert (RIP → RCX, RFLAGS → R11).

### Bug-Hypothese

`syscall_entry.asm` sichert alle Register, aber `sys_handler` oder ein
Syscall-Handler koennte einen callee-saved Register korrumpieren, wenn der
C-Compiler stack-spills falsch handhabt oder ein Handler inline-asm hat.
Der RAX-skip (`add rsp, 8` Zeile 71) koennte off-by-one sein.

### Tests

#### syscall-preserves-rbx-rbp

Lade bekannte Patterns in RBX, RBP, R12-R15 vor dem Syscall. Fuehre
einen harmlosen Syscall (SYS_GETPID) aus. Pruefe alle Register danach.

```c
static void test_syscall_preserves_callee_saved(void) {
    uint64_t rbx_before = 0xAAAAAAAAAAAAAAAAULL;
    uint64_t rbp_before = 0xBBBBBBBBBBBBBBBBULL;
    uint64_t r12_before = 0xCCCCCCCCCCCCCCCCULL;
    uint64_t r13_before = 0xDDDDDDDDDDDDDDDDULL;
    uint64_t r14_before = 0xEEEEEEEEEEEEEEEEULL;
    uint64_t r15_before = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t rbx_after, rbp_after, r12_after, r13_after, r14_after, r15_after;

    __asm__ volatile(
        "mov %[rbx], %%rbx\n"
        "mov %[rbp], %%rbp\n"
        "mov %[r12], %%r12\n"
        "mov %[r13], %%r13\n"
        "mov %[r14], %%r14\n"
        "mov %[r15], %%r15\n"
        "mov $39, %%rax\n"       /* SYS_GETPID */
        "syscall\n"
        "mov %%rbx, %[orbx]\n"
        "mov %%rbp, %[orbp]\n"
        "mov %%r12, %[or12]\n"
        "mov %%r13, %[or13]\n"
        "mov %%r14, %[or14]\n"
        "mov %%r15, %[or15]\n"
        : [orbx] "=r"(rbx_after), [orbp] "=r"(rbp_after),
          [or12] "=r"(r12_after), [or13] "=r"(r13_after),
          [or14] "=r"(r14_after), [or15] "=r"(r15_after)
        : [rbx] "r"(rbx_before), [rbp] "r"(rbp_before),
          [r12] "r"(r12_before), [r13] "r"(r13_before),
          [r14] "r"(r14_before), [r15] "r"(r15_before)
        : "rax", "rcx", "r11", "rdi", "rsi", "rdx", "r10", "r8", "r9", "memory"
    );

    check("rbx preserved", rbx_after == rbx_before);
    check("rbp preserved", rbp_after == rbp_before);
    check("r12 preserved", r12_after == r12_before);
    check("r13 preserved", r13_after == r13_before);
    check("r14 preserved", r14_after == r14_before);
    check("r15 preserved", r15_after == r15_before);
}
```

PASS: alle 6 Register identisch. FAIL: mindestens eins abweichend.

#### syscall-preserves-rsp

RSP vor und nach Syscall vergleichen. Separater Test weil RSP ueber den
percpu user_rsp-Pfad laeuft (nicht ueber die Stack-Pushes).

```c
static void test_syscall_preserves_rsp(void) {
    uint64_t rsp_before, rsp_after;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp_before));
    sc0(SYS_GETPID);
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp_after));
    check("rsp preserved over syscall", rsp_before == rsp_after);
}
```

PASS: RSP identisch. FAIL: RSP verschoben.

#### syscall-preserves-regs-under-load

Wie oben, aber 1000 Iterationen mit SYS_GETPID in einer Schleife, um
seltene Preemption-Korruption zu provozieren. Patterns rotieren pro
Iteration (XOR mit Loop-Counter).

```c
for (int i = 0; i < 1000; i++) {
    uint64_t pattern = 0xDEAD000000000000ULL | (uint64_t)i;
    // Lade pattern in alle callee-saved, syscall, pruefe
}
```

PASS: alle 1000 Iterationen korrekt. FAIL: Abweichung in einer Iteration.

Hinweis: Auf SMP-System kann Preemption zwischen `mov` und `syscall`
auftreten. Der Test provoziert das durch hohe Iteration.

---

## 2. Register-Erhaltung ueber Signale (P0)

### ABI-Invariante

`rt_sigreturn` muss ALLE GPRs (RAX, RBX, RCX, RDX, RSI, RDI, RBP, RSP,
R8-R15) und RIP exakt wiederherstellen, wie sie zum Zeitpunkt der
Signal-Unterbrechung waren. Die Werte stehen im ucontext.gregs auf dem
Signal-Frame.

### Bug-Hypothese

`deliver_signal()` speichert Registers in ucontext. `do_rt_sigreturn()`
liest sie zurueck. Jeder Fehler im Offset-Mapping (sig_gregset_t Indices
vs. thread_t Felder vs. syscall_frame_t Layout) korrumpiert Register.
Der aktuelle Code mappt RAX→frame->rax, aber frame->rax wird vom
syscall_entry.asm Epilog mit dem Return-Value ueberschrieben. Der
Workaround (`return (long)uc.uc_mcontext.gregs.rax`) ist fragil.

### Tests

#### signal-restores-all-gprs

Lade 15 verschiedene Patterns in alle GPRs (ausser RSP). Installiere
SA_SIGINFO-Handler der die Patterns NICHT veraendert (leerer Handler).
Sende SIGUSR1 an sich selbst. Nach Handler-Rueckkehr alle Register
pruefen.

```c
static volatile int handler_ran;

__attribute__((naked)) static void noop_handler(void) {
    __asm__ volatile("incl handler_ran(%rip)\n" "ret\n");
}
```

Problem: Ein C-Handler korrumpiert callee-saved Register durch seinen
Prolog. Deshalb naked-Handler oder SA_SIGINFO-Handler der explizit
nichts tut ausser ein Flag setzen.

Besserer Ansatz — den ucontext im SA_SIGINFO-Handler pruefen:

```c
static void verify_handler(int sig, void *info, void *uctx_) {
    (void)sig; (void)info;
    // uctx_->uc_mcontext.gregs enthaelt die gespeicherten Register.
    // Lese und verifiziere direkt im Handler.
    sig_ucontext_t *uc = (sig_ucontext_t *)uctx_;
    // Pruefe ob gregs.rbx == erwartetes Pattern etc.
    // Setze Ergebnis in globale volatile Variable.
}
```

Der Offset von uc_mcontext.gregs innerhalb ucontext_t muss exakt stimmen
(Offset 40 + gregset-Feld-Offsets). Pruefe mindestens RBX, RBP, R12-R15,
RDI, RSI, RDX, R8-R15, RAX.

PASS: Alle Register im ucontext stimmen mit den geladenen Patterns ueberein
UND nach rt_sigreturn stimmen die Register weiterhin.

FAIL: Ein Register im ucontext ist falsch (deliver_signal Bug) oder nach
rt_sigreturn abweichend (do_rt_sigreturn Bug).

#### signal-restores-rip

Sende Signal. Pruefe dass Ausfuehrung nach dem Signal exakt an der
unterbrochenen Stelle weitergeht (nicht eine Instruktion davor/danach).

```c
volatile int checkpoint = 0;
// Im Handler: nichts tun.
// Im Hauptcode:
checkpoint = 1;
sc2(SYS_KILL, sc0(SYS_GETPID), SIGUSR1); // synchron
checkpoint = 2;
check("rip restored after signal", checkpoint == 2);
```

Feinerer Test: Setze Labels vor/nach Signal und pruefe via RIP im ucontext
dass der Handler an der erwarteten Adresse unterbrochen wurde.

#### nested-signal-register-isolation

Signal A wird geliefert. Waehrend Handler A laeuft, wird Signal B gesendet.
Handler B modifiziert RBX. Nach Rueckkehr aus B muss Handler A sein RBX
unveraendert haben. Nach Rueckkehr aus A muss der Hauptcode sein RBX
unveraendert haben.

Voraussetzung: SA_NODEFER oder unterschiedliche Signal-Nummern (SIGUSR1
fuer A, SIGUSR2 fuer B), damit B nicht durch A's Maske blockiert wird.

```c
// Handler A:
//   Speichere RBX in global_a_rbx
//   Sende SIGUSR2 an sich selbst
//   Pruefe RBX == original Pattern
// Handler B:
//   Setze RBX = 0xBADBADBAD
```

PASS: Handler A's RBX-Check stimmt UND main's RBX-Check stimmt.
FAIL: RBX von Handler B leakt in A oder main.

---

## 3. FPU/SSE-Register ueber Syscalls (P0)

### ABI-Invariante

x86_64 System V ABI: XMM0-XMM15 und MXCSR sind caller-saved, aber der
KERNEL darf sie NICHT veraendern. Der Syscall-Entry/Exit in
`syscall_entry.asm` sichert keine XMM-Register. Das ist nur korrekt,
wenn der Kernel-Code KEINE SSE-Instruktionen nutzt (oder Compiler keine
emittiert). Mit `-mno-sse` im Kernel-Build ist das sicher. OHNE dieses
Flag koennte der Compiler XMM-Register fuer memcpy/struct-copies nutzen.

### Bug-Hypothese

Wenn der Kernel mit SSE kompiliert wird (kein `-mno-sse`), kann jeder
Syscall-Handler XMM-Register korrumpieren. Selbst mit `-mno-sse` koennte
`fxsave`/`fxrstor` im Signal-Code den MXCSR veraendern.

### Tests

#### syscall-preserves-xmm0-15

Lade bekannte 128-bit-Patterns in XMM0-XMM15. Fuehre SYS_GETPID aus.
Pruefe alle 16 XMM-Register.

```c
static void test_syscall_preserves_xmm(void) {
    // 16 verschiedene 128-bit Patterns
    __attribute__((aligned(16))) uint8_t before[16][16];
    __attribute__((aligned(16))) uint8_t after[16][16];

    // Fuelle before[] mit bekannten Werten
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            before[i][j] = (uint8_t)(i * 16 + j);

    __asm__ volatile(
        "movdqa  0*16(%[b]), %%xmm0\n"  "movdqa  1*16(%[b]), %%xmm1\n"
        "movdqa  2*16(%[b]), %%xmm2\n"  "movdqa  3*16(%[b]), %%xmm3\n"
        "movdqa  4*16(%[b]), %%xmm4\n"  "movdqa  5*16(%[b]), %%xmm5\n"
        "movdqa  6*16(%[b]), %%xmm6\n"  "movdqa  7*16(%[b]), %%xmm7\n"
        "movdqa  8*16(%[b]), %%xmm8\n"  "movdqa  9*16(%[b]), %%xmm9\n"
        "movdqa 10*16(%[b]), %%xmm10\n" "movdqa 11*16(%[b]), %%xmm11\n"
        "movdqa 12*16(%[b]), %%xmm12\n" "movdqa 13*16(%[b]), %%xmm13\n"
        "movdqa 14*16(%[b]), %%xmm14\n" "movdqa 15*16(%[b]), %%xmm15\n"
        "mov $39, %%rax\n"              /* SYS_GETPID */
        "syscall\n"
        "movdqa %%xmm0,   0*16(%[a])\n" "movdqa %%xmm1,   1*16(%[a])\n"
        "movdqa %%xmm2,   2*16(%[a])\n" "movdqa %%xmm3,   3*16(%[a])\n"
        "movdqa %%xmm4,   4*16(%[a])\n" "movdqa %%xmm5,   5*16(%[a])\n"
        "movdqa %%xmm6,   6*16(%[a])\n" "movdqa %%xmm7,   7*16(%[a])\n"
        "movdqa %%xmm8,   8*16(%[a])\n" "movdqa %%xmm9,   9*16(%[a])\n"
        "movdqa %%xmm10, 10*16(%[a])\n" "movdqa %%xmm11, 11*16(%[a])\n"
        "movdqa %%xmm12, 12*16(%[a])\n" "movdqa %%xmm13, 13*16(%[a])\n"
        "movdqa %%xmm14, 14*16(%[a])\n" "movdqa %%xmm15, 15*16(%[a])\n"
        :: [b] "r"(before), [a] "r"(after)
        : "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7",
          "xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15",
          "rax","rcx","r11","memory"
    );

    int ok = 1;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            if (before[i][j] != after[i][j]) ok = 0;
    check("xmm0-15 preserved over syscall", ok);
}
```

PASS: alle 256 Bytes identisch. FAIL: XMM-Korruption durch Kernel-Code.

#### syscall-preserves-mxcsr

Setze MXCSR auf spezifischen Wert (z.B. Round-to-Nearest + alle Masks
gesetzt = 0x1F80). Fuehre Syscall aus. Lese MXCSR zurueck. Vergleiche.

```c
uint32_t mxcsr_before = 0x1F80;
uint32_t mxcsr_after;
__asm__ volatile("ldmxcsr %0" :: "m"(mxcsr_before));
sc0(SYS_GETPID);
__asm__ volatile("stmxcsr %0" : "=m"(mxcsr_after));
check_val("mxcsr preserved", (long)mxcsr_after, (long)mxcsr_before);
```

Wiederhole mit verschiedenen Rounding-Modes (0x1F80, 0x3F80, 0x5F80, 0x7F80).

---

## 4. FPU/SSE-Register ueber Signale (P0)

### ABI-Invariante

Signal-Delivery muss FPU/SSE-State via FXSAVE sichern, rt_sigreturn
muss via FXRSTOR wiederherstellen. Der State befindet sich in
ucontext.__fpregs_mem (sig_fpstate_t, 512 Bytes FXSAVE-Bereich).

### Bug-Hypothese

`deliver_signal()` macht FXSAVE in den aktuellen Kernel-FPU-State, nicht
den User-FPU-State. Wenn der Kernel zwischen Syscall-Entry und
Signal-Delivery den FPU-State veraendert hat (z.B. durch FXSAVE/FXRSTOR
fuer einen anderen Zweck), ist der gesicherte State falsch.

Ausserdem: FXSAVE-Alignment. Der Code nutzt einen aligned Temp-Buffer,
aber wenn die Alignment-Attribute nicht korrekt sind, koennte FXSAVE
eine #GP auslösen.

### Tests

#### signal-preserves-xmm

Lade Patterns in XMM0-XMM7 (reichen fuer die Pruefung). Sende Signal.
Handler ist leer (oder prueft im ucontext). Nach Rueckkehr: XMM pruefen.

```c
static volatile int xmm_handler_ran;
static void xmm_handler(int sig) { (void)sig; xmm_handler_ran = 1; }

static void test_signal_preserves_xmm(void) {
    __attribute__((aligned(16))) uint8_t before[8][16];
    __attribute__((aligned(16))) uint8_t after[8][16];

    // Fuelle before mit Patterns
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 16; j++)
            before[i][j] = (uint8_t)(0xA0 + i * 16 + j);

    // Installiere Handler
    struct ksigaction_b sa = { .handler = (void *)xmm_handler,
        .flags = SA_RESTORER, .restorer = (void *)sig_restorer_b, .mask = 0 };
    sc4(SYS_RT_SIGACTION, SIGUSR1, (long)&sa, 0, 8);

    __asm__ volatile(
        "movdqa 0*16(%[b]), %%xmm0\n" "movdqa 1*16(%[b]), %%xmm1\n"
        "movdqa 2*16(%[b]), %%xmm2\n" "movdqa 3*16(%[b]), %%xmm3\n"
        "movdqa 4*16(%[b]), %%xmm4\n" "movdqa 5*16(%[b]), %%xmm5\n"
        "movdqa 6*16(%[b]), %%xmm6\n" "movdqa 7*16(%[b]), %%xmm7\n"
        :: [b] "r"(before)
        : "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7","memory"
    );

    sc2(SYS_KILL, sc0(SYS_GETPID), SIGUSR1);

    __asm__ volatile(
        "movdqa %%xmm0, 0*16(%[a])\n" "movdqa %%xmm1, 1*16(%[a])\n"
        "movdqa %%xmm2, 2*16(%[a])\n" "movdqa %%xmm3, 3*16(%[a])\n"
        "movdqa %%xmm4, 4*16(%[a])\n" "movdqa %%xmm5, 5*16(%[a])\n"
        "movdqa %%xmm6, 6*16(%[a])\n" "movdqa %%xmm7, 7*16(%[a])\n"
        :: [a] "r"(after)
        : "memory"
    );

    int ok = 1;
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 16; j++)
            if (before[i][j] != after[i][j]) ok = 0;
    check("xmm0-7 preserved over signal", ok);
}
```

PASS: XMM identisch. FAIL: FXSAVE/FXRSTOR Pfad defekt.

#### signal-preserves-mxcsr

Wie MXCSR-Test ueber Syscall, aber mit Signal dazwischen.

#### signal-handler-can-use-xmm

Handler modifiziert XMM0 absichtlich (z.B. `xorps %xmm0, %xmm0`).
Nach Rueckkehr muss XMM0 den pre-Signal-Wert haben (nicht Null).
Testet dass rt_sigreturn den FPU-State korrekt wiederherstellt, auch
wenn der Handler ihn veraendert.

---

## 5. FPU/SSE-Register ueber Context-Switch (P0)

### ABI-Invariante

Jeder Thread hat seinen eigenen FPU/SSE-State. Context-Switches muessen
den State sichern und wiederherstellen.

### Bug-Hypothese

**KRITISCH**: Der aktuelle Code (`sched_preempt`, `thread_run`) sichert
KEINE FPU/SSE-Register beim Context-Switch. Es gibt kein `fxsave`/
`fxrstor` in `sched_preempt()` oder `thread_run()`. Nur signal
delivery/return macht FXSAVE/FXRSTOR.

Das bedeutet: Zwei Threads die gleichzeitig XMM-Register nutzen sehen
den FPU-State des jeweils anderen nach einem Context-Switch.

### Tests

#### context-switch-xmm-isolation

Zwei Threads. Thread A setzt XMM0 = 0xAAAA..., Thread B setzt
XMM0 = 0xBBBB... . Beide loopen und pruefen ihren Wert. Nach einigen
Context-Switches (SYS_SCHED_YIELD oder busy-wait) pruefen beide ob
ihr XMM0-Wert intakt ist.

```c
static volatile int thread_a_ok = 1;
static volatile int thread_b_ok = 1;
static volatile int ab_sync = 0;

static void thread_a_fn(void) {
    __attribute__((aligned(16))) uint8_t pat[16];
    for (int i = 0; i < 16; i++) pat[i] = 0xAA;
    __asm__ volatile("movdqa (%0), %%xmm0" :: "r"(pat) : "xmm0");

    __sync_fetch_and_add((int *)&ab_sync, 1);
    for (int i = 0; i < 100000; i++) {
        __attribute__((aligned(16))) uint8_t cur[16];
        __asm__ volatile("movdqa %%xmm0, (%0)" :: "r"(cur) : "memory");
        for (int j = 0; j < 16; j++)
            if (cur[j] != 0xAA) { thread_a_ok = 0; break; }
        __asm__ volatile("pause");
    }
    sc1(SYS_EXIT, 0);
}

static void thread_b_fn(void) {
    __attribute__((aligned(16))) uint8_t pat[16];
    for (int i = 0; i < 16; i++) pat[i] = 0xBB;
    __asm__ volatile("movdqa (%0), %%xmm0" :: "r"(pat) : "xmm0");

    __sync_fetch_and_add((int *)&ab_sync, 1);
    for (int i = 0; i < 100000; i++) {
        __attribute__((aligned(16))) uint8_t cur[16];
        __asm__ volatile("movdqa %%xmm0, (%0)" :: "r"(cur) : "memory");
        for (int j = 0; j < 16; j++)
            if (cur[j] != 0xBB) { thread_b_ok = 0; break; }
        __asm__ volatile("pause");
    }
    sc1(SYS_EXIT, 0);
}
```

Spawne A und B via SYS_CLONE (CLONE_VM | CLONE_THREAD...), jeder mit
eigenem Stack. Warte auf beide (busy-wait auf ab_sync oder Futex).

PASS: thread_a_ok == 1 && thread_b_ok == 1.
FAIL: XMM0-Korruption. **Erwartet FAIL mit aktuellem Code.**

Hinweis: Braucht SMP oder Preemption. Auf Uniprocessor-QEMU ohne Timer
koennte kein Preemption stattfinden. `make test-hw` mit `-smp 2` noetig.

#### context-switch-mxcsr-isolation

Thread A setzt MXCSR = Round-Nearest (0x1F80).
Thread B setzt MXCSR = Round-Down (0x3F80).
Beide pruefen in Schleife ob ihr MXCSR-Wert intakt ist.

Gleiche Infrastruktur wie oben. **Erwartet FAIL.**

---

## 6. TLS (FS_BASE) ueber Context-Switch, Signale, fork, clone (P0)

### ABI-Invariante

FS_BASE (MSR 0xC0000100) ist per-Thread. Jeder Thread setzt es via
`arch_prctl(ARCH_SET_FS, addr)`. Der Kernel muss es bei jedem
Context-Switch sichern (RDMSR) und wiederherstellen (WRMSR).

### Bug-Hypothese

`sched_preempt()` sichert FS_BASE via RDMSR (Zeile 371-373), aber
`thread_run()` stellt es nur wieder her wenn `t->fs_base != 0`
(Zeile 363). Ein Thread der FS_BASE = 0 hat (kein TLS) wuerde den
FS_BASE des vorherigen Threads erben. Das ist korrekt solange es nur
einen TLS-nutzenden Thread gibt, wird aber bei mehreren Threads mit
verschiedenen TLS-Adressen zum Problem.

### Tests

#### tls-cross-thread-isolation

Thread A setzt FS_BASE = 0x700000000000.
Thread B setzt FS_BASE = 0x700000001000.
Beide lesen FS_BASE in Schleife und pruefen.

```c
static void tls_thread_fn(void) {
    uint64_t expected = /* aus arg (auf Stack oder globaler Slot) */;
    sc2(SYS_ARCH_PRCTL, ARCH_SET_FS, expected);

    for (int i = 0; i < 10000; i++) {
        uint64_t fs;
        sc2(SYS_ARCH_PRCTL, ARCH_GET_FS, (long)&fs);
        if (fs != expected) { /* Fehler signalisieren */ }
        __asm__ volatile("pause");
    }
    sc1(SYS_EXIT, 0);
}
```

PASS: Kein Thread sieht den FS_BASE des anderen.
FAIL: FS_BASE-Korruption nach Preemption.

#### tls-survives-signal

Setze FS_BASE. Sende Signal. Pruefe FS_BASE nach Signal-Rueckkehr.

Relevanz: Signal-Delivery/Return veraendert das syscall_frame, aber
FS_BASE wird separat ueber MSR verwaltet. Wenn `do_rt_sigreturn` den
MSR nicht restauriert, koennte FS_BASE nach Signal abweichen.

Aktueller Code: `do_rt_sigreturn` schreibt NICHT den FS_BASE MSR.
Er restauriert nur die GPRs. Wenn zwischen Signal-Entry und -Return
ein Context-Switch stattfand der den MSR auf einen anderen Thread's
Wert setzte, ist FS_BASE nach rt_sigreturn falsch.

**Erwartet FAIL unter Concurrency.**

#### tls-preserved-over-fork

Parent setzt FS_BASE = X. fork(). Child prueft FS_BASE.

Aktueller Code: `do_fork` kopiert cur->fs_base nicht explizit ins Child.
Child startet via `proc_enter_ring3` → FS_BASE wird in `thread_run`
nur gesetzt wenn `t->fs_base != 0`. Aber fs_base wird in `do_fork` nicht
kopiert (nur GPRs werden kopiert).

Pruefe ob `ct->fs_base` gesetzt wird. Zeile ~716 in process.c zeigt
`save_user_state_for_block(cur, 0)` — das speichert User-Register in
cur, aber ob fs_base auch kopiert wird, muss geprueft werden.

```c
sc2(SYS_ARCH_PRCTL, ARCH_SET_FS, 0x7F0000000000ULL);
long pid = sc0(SYS_FORK);
if (pid == 0) {
    uint64_t fs;
    sc2(SYS_ARCH_PRCTL, ARCH_GET_FS, (long)&fs);
    // Pruefe fs == 0x7F0000000000ULL, schreibe Ergebnis via pipe oder exit-code
    sc1(SYS_EXIT_GROUP, (fs == 0x7F0000000000ULL) ? 0 : 1);
}
// Parent: wait4, pruefe exit_code == 0
```

PASS: Child hat gleichen FS_BASE. FAIL: FS_BASE ist 0 oder falsch.

#### tls-clone-settls

clone(CLONE_SETTLS, tls=0x700000002000). Child liest FS_BASE.
Muss 0x700000002000 sein.

```c
long tid = sc5(SYS_CLONE,
    (long)(CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|CLONE_SETTLS),
    stack + 65536, 0, 0, 0x700000002000ULL);
```

Im Child-Thread:
```c
uint64_t fs;
sc2(SYS_ARCH_PRCTL, ARCH_GET_FS, (long)&fs);
// fs muss 0x700000002000 sein
```

---

## 7. Stack-Alignment (P0)

### ABI-Invariante

x86_64 System V ABI: RSP ≡ 0 (mod 16) bei Prozessstart und an jedem
CALL-Punkt. Am Eintritt einer Funktion (nach CALL, das 8 Bytes pusht):
RSP ≡ 8 (mod 16).

Signal-Handler-Eintritt: RSP ≡ 8 (mod 16) — simuliert einen CALL auf
einen 16-aligned Stack.

### Bug-Hypothese

Bereits als BUG-SIG2 gefunden und gefixt. Aber: Alignment nach fork()
und nach clone() nicht geprueft. Wenn proc_enter_ring3 den Stack falsch
aufbaut, koennte das Alignment in neuen Prozessen/Threads kaputt sein.

### Tests

#### fork-child-rsp-alignment

Fork. Im Child: RSP auslesen und Alignment pruefen.

```c
long pid = sc0(SYS_FORK);
if (pid == 0) {
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    // RSP sollte im selben Alignment sein wie im Parent
    sc1(SYS_EXIT_GROUP, (rsp & 0xF) == 0 ? 0 : 1);
}
```

Hinweis: Exaktes Alignment haengt vom ABI-Punkt ab (main vs. nach CALL).
Der Test muss den Kontext beruecksichtigen.

#### clone-child-rsp-alignment

Clone mit explizitem child_stack. Der Stack-Pointer muss 16-aligned sein
am Eintritt des Child-Codes.

```c
long stk = sc6(SYS_MMAP, 0, 65536, PROT_RW, MAP_PRIV_ANON, -1, 0);
long tid = sc5(SYS_CLONE, flags, stk + 65536, 0, 0, 0);
if (tid == 0) {
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    // stk + 65536 ist 16-aligned (mmap gibt Page-aligned zurueck)
    // RSP muss 16-aligned sein (proc_enter_ring3 IRET)
}
```

PASS: RSP & 0xF == 0. FAIL: Misalignment.

#### signal-handler-alignment-stress

100 Signale in schneller Folge. Jeder Handler prueft RSP-Alignment.
Zaehle Fehlschlaege.

```c
static volatile int align_fails;
static void align_handler(int sig) {
    (void)sig;
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    // Nach push rbp (Compiler-Prolog): RSP mod 16 == 0
    if (rsp & 0xF) __sync_fetch_and_add((int *)&align_fails, 1);
}
```

PASS: align_fails == 0. FAIL: Intermittierendes Alignment-Problem.

---

## 8. Signal-Semantik (P1)

### ABI-Invariante

Linux Signal-Semantik: blockierende Syscalls returnen -EINTR wenn ein
Signal geliefert wird. SA_RESTART bewirkt automatischen Syscall-Restart.
Signale die geblockt sind werden pending und bei Unblock geliefert.
sa_mask wird waehrend Handler-Ausfuehrung zur Signal-Maske addiert.

### Tests

#### eintr-on-blocked-read

Erstelle Pipe. Starte read() auf Lese-Ende (blockiert, da leer).
Sende Signal. read() muss -EINTR returnen.

```c
int fds[2];
sc1(SYS_PIPE, (long)fds);

// Fork: Child blockiert in read(fds[0])
long pid = sc0(SYS_FORK);
if (pid == 0) {
    // Installiere SIGUSR1-Handler (ohne SA_RESTART)
    struct ksigaction_b sa = { .handler = handler, .flags = SA_RESTORER,
        .restorer = restorer, .mask = 0 };
    sc4(SYS_RT_SIGACTION, SIGUSR1, (long)&sa, 0, 8);

    char buf;
    long r = sc3(SYS_READ, fds[0], (long)&buf, 1);
    // r muss -EINTR sein
    sc1(SYS_EXIT_GROUP, (r == -EINTR) ? 0 : 1);
}
// Parent: kurz warten, dann kill(pid, SIGUSR1), dann wait4
```

PASS: Child exit-code 0 (read returned -EINTR).
FAIL: read haengt oder returnt anderen Wert.

#### sa-restart-restarts-read

Wie oben, aber Handler mit SA_RESTART-Flag. read() darf NICHT -EINTR
returnen sondern muss re-starten. Parent schreibt danach ein Byte in
die Pipe. read() muss das Byte lesen und mit 1 returnen.

Implementierungsdetail: `check_signals_syscall_path()` prueft
`is_restartable_syscall()` und rewinded RIP um 2 Bytes. Dieser Rewind
muss korrekt sein (SYSCALL = 0F 05 = 2 Bytes).

#### pending-signal-delivered-on-unblock

Block SIGUSR1. Sende SIGUSR1 zweimal. Unblock SIGUSR1. Handler muss
genau einmal laufen (Signale sind nicht queued, nur ein Bit im
sig_pending).

```c
uint64_t mask = 1ULL << SIGUSR1;
sc4(SYS_RT_SIGPROCMASK, 0 /* SIG_BLOCK */, (long)&mask, 0, 8);
sc2(SYS_KILL, sc0(SYS_GETPID), SIGUSR1);
sc2(SYS_KILL, sc0(SYS_GETPID), SIGUSR1);
check("handler not called while blocked", handler_count == 0);
sc4(SYS_RT_SIGPROCMASK, 1 /* SIG_UNBLOCK */, (long)&mask, 0, 8);
check_val("handler called once after unblock", handler_count, 1);
```

#### sa-mask-blocks-during-handler

Installiere SIGUSR1-Handler mit sa_mask = (1<<SIGUSR2). Im Handler:
pruefe dass SIGUSR2 blockiert ist (lese sig_blocked via
rt_sigprocmask). Nach Handler: SIGUSR2 muss wieder unblocked sein.

```c
static void mask_handler(int sig) {
    (void)sig;
    uint64_t blocked;
    sc4(SYS_RT_SIGPROCMASK, 2 /* SIG_SETMASK — actually just read */, 0,
        (long)&blocked, 8);
    // Pruefe: (1ULL << SIGUSR2) muss in blocked gesetzt sein
}
```

Hinweis: rt_sigprocmask mit how=SIG_SETMASK und set=NULL gibt nur
oldset zurueck, veraendert aber nichts. Oder besser: how=0, set=NULL,
oldset = ptr. Der aktuelle Code prueft `if (set)` vor Modifikation.

#### sigsuspend-semantics

Pruefe: rt_sigsuspend blockiert, Signal weckt auf, Rueckgabewert -EINTR,
alte Maske ist wiederhergestellt.

```c
uint64_t empty_mask = 0;  // keine Signale blockiert
// In einem Thread: sigsuspend(empty_mask)
// Anderer Thread oder Timer: SIGUSR1 senden
// Erwartung: sigsuspend returnt -EINTR, Handler wurde aufgerufen,
// sig_blocked ist auf pre-sigsuspend-Wert zurueck.
```

---

## 9. fork/clone Semantik unter Stress (P1)

### ABI-Invariante

fork(): Kind erbt Kopie aller Register, Address-Space, FD-Tabelle,
Signal-Handler, Signal-Maske, CWD. Kind hat eigenen PID, PPID = Parent.
fork() returnt 0 im Kind, Kind-PID im Parent.

clone(CLONE_VM|CLONE_THREAD): Geteilter Address-Space, geteilte
FD-Tabelle, geteilte Signal-Handler. Eigener Stack, eigene Signal-Maske
(geerbt), eigener TID.

### Tests

#### fork-register-fidelity

Setze alle callee-saved auf Patterns. fork(). Im Child: pruefe alle
Register. Im Parent: pruefe dass eigene Register unveraendert sind.

```c
// Inline-ASM: Lade Patterns → fork-Syscall → pruefe im Child
// Child: exit(0) wenn alle korrekt, exit(1) sonst
// Parent: wait4 → exit-code pruefen UND eigene Register pruefen
```

#### fork-address-space-isolation

Parent mmapt eine Page, schreibt Pattern. fork(). Kind liest — muss
Pattern sehen. Kind schreibt anderes Pattern. Parent liest — muss
immer noch Original sehen (CoW oder Deep-Copy).

```c
long pg = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
*(uint64_t *)pg = 0xCAFEBABE;
long pid = sc0(SYS_FORK);
if (pid == 0) {
    uint64_t val = *(uint64_t *)pg;
    if (val != 0xCAFEBABE) sc1(SYS_EXIT_GROUP, 1);
    *(uint64_t *)pg = 0xDEADBEEF;
    sc1(SYS_EXIT_GROUP, 0);
}
sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
check("child saw parent data", WEXITSTATUS(wstatus) == 0);
check("parent data not modified by child", *(uint64_t *)pg == 0xCAFEBABE);
```

#### fork-fd-independence

Parent oeffnet Datei. fork(). Kind schliesst FD. Parent muss FD immer
noch nutzen koennen.

#### clone-concurrent-stress

Spawne 8 Threads via clone. Jeder Thread inkrementiert einen shared
Atomic-Counter 10000 Mal. Am Ende: Counter == 80000.

```c
static volatile int shared_counter;
static void clone_worker(void) {
    for (int i = 0; i < 10000; i++)
        __sync_fetch_and_add((int *)&shared_counter, 1);
    sc1(SYS_EXIT, 0);
}
```

PASS: shared_counter == 80000.
FAIL: Zaehler zu klein (Preemption-Bug) oder Crash (Stack-Korruption).

Hinweis: Stack fuer jeden Thread separat mmap-en (je 64KB).

#### clone-child-tid-futex

clone(CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID). Pruefe:
- parent_tid wird im Parent gesetzt
- child_tid wird im Kind gesetzt (selber Wert)
- Bei Kind-Exit: *child_tid wird zu 0, FUTEX_WAKE wird gemacht

```c
int parent_tid_val = 0, child_tid_val = 0;
long tid = sc5(SYS_CLONE,
    (long)(CLONE_VM|CLONE_THREAD|CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID|...),
    stack + 65536, (long)&parent_tid_val, (long)&child_tid_val, 0);
// Child: exit sofort
// Parent: futex_wait(&child_tid_val, tid, ...) oder busy-wait bis child_tid_val == 0
check("child_tid cleared on exit", child_tid_val == 0);
check_val("parent_tid set", parent_tid_val, (int)tid);
```

---

## 10. mmap Edge-Cases (P1)

### ABI-Invariante

mmap: MAP_FIXED ueberschreibt existierende Mappings. MAP_FIXED_NOREPLACE
returnt -EEXIST wenn Overlap. PROT_NONE verbietet jeden Zugriff (nicht
mal lesbar). mprotect aendert Permissions bestehender Mappings.

### Tests

#### mmap-fixed-replaces

Mappe 2 Pages. Mappe MAP_FIXED ueber die zweite Page. Pruefe dass die
zweite Page die neuen Daten hat und die erste unveraendert ist.

```c
long base = sc6(SYS_MMAP, 0, 8192, PROT_RW, MAP_PRIV_ANON, -1, 0);
*(uint64_t *)base = 0x1111;
*(uint64_t *)(base + 4096) = 0x2222;

long r = sc6(SYS_MMAP, base + 4096, 4096, PROT_RW,
             MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
check_val("MAP_FIXED returns same addr", r, base + 4096);
check("page 1 untouched", *(uint64_t *)base == 0x1111);
check("page 2 zeroed by new mapping", *(uint64_t *)(base + 4096) == 0);
```

#### mmap-fixed-noreplace

Mappe eine Page. Versuche MAP_FIXED_NOREPLACE auf selbe Adresse.
Muss -EEXIST returnen.

```c
long pg = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
long r = sc6(SYS_MMAP, pg, 4096, PROT_RW,
             MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
check_val("MAP_FIXED_NOREPLACE → -EEXIST", r, -EEXIST);
sc2(SYS_MUNMAP, pg, 4096);
```

#### mprotect-prot-none-faults

Mappe Page RW. Schreibe Daten. mprotect zu PROT_NONE. Lesen muss
SIGSEGV ausloesen. mprotect zurueck zu PROT_READ. Lesen muss
funktionieren und alte Daten enthalten.

```c
// Fork um SIGSEGV zu testen ohne den Testprozess zu toeten
long pg = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
*(volatile uint64_t *)pg = 0xCAFE;
sc3(SYS_MPROTECT, pg, 4096, PROT_NONE);

long pid = sc0(SYS_FORK);
if (pid == 0) {
    volatile uint64_t v = *(volatile uint64_t *)pg; // SIGSEGV
    (void)v;
    sc1(SYS_EXIT_GROUP, 0); // sollte nicht erreicht werden
}
int wstatus;
sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
check("PROT_NONE causes SIGSEGV", WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == SIGSEGV);

sc3(SYS_MPROTECT, pg, 4096, PROT_READ);
check("data preserved after PROT_NONE→PROT_READ", *(volatile uint64_t *)pg == 0xCAFE);
```

#### mmap-large-mapping

Mappe 16MB anonym. Schreibe an erste und letzte Page. Lese zurueck.
Testet ob grosse VMA-Bereiche korrekt verwaltet werden.

```c
size_t sz = 16 * 1024 * 1024;
long base = sc6(SYS_MMAP, 0, sz, PROT_RW, MAP_PRIV_ANON, -1, 0);
check("16MB mmap", base > 0);
*(volatile uint64_t *)base = 0xAAAA;
*(volatile uint64_t *)(base + sz - 8) = 0xBBBB;
check("first page", *(volatile uint64_t *)base == 0xAAAA);
check("last page", *(volatile uint64_t *)(base + sz - 8) == 0xBBBB);
sc2(SYS_MUNMAP, base, sz);
```

#### mremap-move

Mappe 4KB, schreibe Pattern. mremap zu 8KB mit MREMAP_MAYMOVE. Pruefe
dass alter Inhalt erhalten ist und neue Bytes null sind.

---

## 11. Timing/Race-Tests (P0)

### ABI-Invariante

Signale muessen korrekt geliefert werden waehrend blockierender Syscalls.
Context-Switches duerfen keinen State korrumpieren. Preemption waehrend
Signal-Delivery darf den Signal-Frame nicht zerstoeren.

### Bug-Hypothese

Race zwischen `check_signals_syscall_path()` und Timer-IRQ:
Signal wird auf dem SYSCALL-Return-Pfad geliefert. Timer-IRQ preempted
zwischen Frame-Manipulation und SYSRET. Der IRQ-Handler sieht einen
inkonsistenten Frame.

### Tests

#### signal-during-blocking-syscall

Thread A blockiert in read() (leere Pipe). Thread B sendet Signal.
Thread A muss aufwachen und -EINTR bekommen.

Implementierung: Wie eintr-on-blocked-read (Abschnitt 8), aber mit
echtem Concurrency statt fork.

```c
// Thread A: read(pipe_rd, buf, 1) → blockiert
// Thread B: nanosleep(10ms), dann tgkill(pid, tid_a, SIGUSR1)
// Thread A: read muss -EINTR returnen
```

Hinweis: Braucht SMP oder Preemption damit B laeuft waehrend A blockiert.

#### signal-storm-no-corruption

Sende 1000 Signale in schneller Folge. Handler zaehlt Aufrufe.
Pruefe dass alle Register nach dem Sturm intakt sind.

```c
static volatile int storm_count;
static void storm_handler(int sig) { (void)sig; storm_count++; }

// Lade Patterns in callee-saved
for (int i = 0; i < 1000; i++)
    sc2(SYS_KILL, sc0(SYS_GETPID), SIGUSR1);
// Pruefe callee-saved Register
// storm_count muss > 0 sein (nicht alle muessen einzeln geliefert werden)
```

PASS: Register intakt, storm_count > 0.
FAIL: Register-Korruption oder Crash.

#### preemption-during-fork

Spawne 4 Threads die alle gleichzeitig fork() aufrufen. Pruefe dass
alle fork-Kinder korrekt starten und beenden.

Testet die Thread-Suspendierung in do_fork() unter Concurrency
(Zeile 629-643 in process.c). Race: Thread C wird suspended waehrend
es selbst gerade in einem Syscall ist.

```c
static volatile int fork_ok_count;
static void fork_worker(void) {
    long pid = sc0(SYS_FORK);
    if (pid == 0) { sc1(SYS_EXIT_GROUP, 0); __builtin_unreachable(); }
    if (pid > 0) {
        int ws;
        sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
        if (WIFEXITED(ws) && WEXITSTATUS(ws) == 0)
            __sync_fetch_and_add((int *)&fork_ok_count, 1);
    }
    sc1(SYS_EXIT, 0);
}
```

PASS: fork_ok_count == 4. FAIL: Haengt, crasht, oder Kind startet nicht.

#### concurrent-signal-and-preemption

Thread A: Endlosschleife mit XMM-Patterns + Pruefen.
Thread B: Sendet SIGUSR1 an Thread A in Schleife.
Handler: Modifiziert XMM0 absichtlich.
Prueft: Thread A's XMM-Werte werden durch Signal + rt_sigreturn
korrekt wiederhergestellt, auch unter Preemption.

Kombiniert Kategorien 4, 5 und 11. Erwartet FAIL wegen fehlendem
FPU-Save/Restore im Context-Switch.

---

## 12. Errno/Return-Value-Korrektheit unter Concurrency (P2)

### ABI-Invariante

Linux-Syscalls returnen negative errno-Werte in RAX bei Fehler.
Jeder Thread hat seinen eigenen RAX nach SYSRET. Es gibt kein globales
errno im Kernel.

### Bug-Hypothese

Wenn `sys_handler` das Ergebnis in RAX zurueckgibt und dann
`check_signals_syscall_path` es ueber `*result_ptr` modifiziert,
koennte ein Race existieren wo der Pointer auf Stack-Speicher zeigt
der durch einen IRQ ueberschrieben wird.

### Tests

#### concurrent-syscall-errno

4 Threads. Jeder ruft einen fehlschlagenden Syscall auf (z.B.
`openat(-1, "/nonexistent", 0, 0)` → -ENOENT). Prueft den
Return-Value 1000 Mal.

```c
static void errno_worker(void) {
    for (int i = 0; i < 1000; i++) {
        long r = sc4(SYS_OPENAT, -100, (long)"/nonexistent", 0, 0);
        if (r != -ENOENT) { /* Fehler melden */ }
    }
    sc1(SYS_EXIT, 0);
}
```

PASS: Alle 4000 Aufrufe returnen exakt -ENOENT.
FAIL: Ein Return-Value weicht ab (z.B. -EINTR statt -ENOENT, oder 0).

#### fork-return-value-correctness

Fork 10 Mal hintereinander. Pruefe dass Parent immer Kind-PID > 0
bekommt und Kind immer 0. Keine Verwechslung.

```c
for (int i = 0; i < 10; i++) {
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        long my_pid = sc0(SYS_GETPID);
        // my_pid muss != parent_pid sein
        sc1(SYS_EXIT_GROUP, 0);
    }
    check("fork returned child pid", pid > 0);
    int ws;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
}
```

---

## Bug-Wahrscheinlichkeit nach Prioritaet

| P  | Kategorie                      | Erwartet FAIL? | Begruendung |
|----|--------------------------------|----------------|-------------|
| P0 | 5. XMM ueber Context-Switch    | JA             | Kein FXSAVE/FXRSTOR in sched_preempt/thread_run |
| P0 | 6. TLS unter Signal+Preemption | JA             | rt_sigreturn stellt FS_BASE MSR nicht her |
| P0 | 11. Signal+Preemption+XMM      | JA             | Kombination aus 4+5 |
| P0 | 3. XMM ueber Syscall           | Moeglich       | Abhaengig von Compiler-Flags (-mno-sse?) |
| P0 | 1. GPR ueber Syscall           | Unwahrscheinl. | syscall_entry.asm sieht korrekt aus |
| P0 | 2. GPR ueber Signal            | Moeglich       | Offset-Mapping ist komplex |
| P0 | 4. XMM ueber Signal            | Moeglich       | FXSAVE im Kernel-Context, nicht User-Context |
| P0 | 7. Stack-Alignment             | Unwahrscheinl. | Bereits gefixt (BUG-SIG2) |
| P1 | 8. Signal-Semantik             | Moeglich       | SA_RESTART-Rewind-Logik komplex |
| P1 | 9. fork/clone Stress           | Moeglich       | Thread-Suspendierung in fork Race |
| P1 | 10. mmap Edge-Cases            | Moeglich       | VMA-Splitting-Logik komplex |
| P2 | 12. Errno unter Concurrency    | Unwahrscheinl. | Jeder Thread hat eigenen Frame |

## Implementierungsreihenfolge

1. **test_abi_xmm_context_switch** (Kat. 5) — findet den offensichtlichsten Bug
2. **test_abi_tls_isolation** (Kat. 6) — zweiter offensichtlicher Bug
3. **test_abi_syscall_regs** (Kat. 1) — Baseline-Verifikation
4. **test_abi_xmm_syscall** (Kat. 3) — zeigt Compiler-Flag-Problem
5. **test_abi_signal_regs** (Kat. 2) — dritte Bug-Klasse
6. **test_abi_xmm_signal** (Kat. 4) — FPU ueber Signal
7. **test_abi_alignment** (Kat. 7) — Regression-Schutz
8. **test_abi_signal_semantics** (Kat. 8) — EINTR/RESTART
9. **test_abi_fork_clone** (Kat. 9) — fork Stress
10. **test_abi_mmap** (Kat. 10) — mmap Edge-Cases
11. **test_abi_timing** (Kat. 11) — Race-Tests
12. **test_abi_errno** (Kat. 12) — Concurrency-Errno

## Infrastruktur-Anforderungen

- `make test-hw` mit `-smp 2` (mindestens) fuer Preemption-Tests
- Timer-IRQ muss aktiv sein (100ms Tick fuer Preemption)
- Genug freier Speicher fuer 16MB-mmap-Test
- Pipe-Support fuer EINTR-Tests
- clone/CLONE_VM Support fuer alle Thread-Tests
