# CosmoRT SIMD Design

Multi-Arch SIMD-Support: x86_64, ARM64, WASM.

## Ist-Zustand

| Aspekt | Status |
|--------|--------|
| FPU-State Save/Restore | FXSAVE/FXRSTOR (512 Bytes, 16-Byte Alignment) |
| Context-Switch | Eager: FXSAVE in `sched_preempt`, FXRSTOR in `thread_run` |
| fork | `kmemcpy(ct->fxsave_area, cur->fxsave_area, 512)` |
| exec | `kmemset` + MXCSR 0x1F80 |
| Kernel SIMD | Implizit erlaubt (kein `-mno-sse` in KCFLAGS) |
| memops | Reines C, word-at-a-time (8 Bytes/Iteration) |
| CPUID Detection | ERMS, RDRAND vorhanden; AVX2-Bit gelesen aber ungenutzt |
| `fxsave_area` in `thread_t` | `uint8_t[512] aligned(16)`, am Ende der Struct |

Problem: Der Kernel kompiliert ohne `-mno-sse`. GCC darf SSE-Register in beliebigem
Kernel-Code nutzen (Auto-Vektorisierung, Register Spilling). Jeder Syscall-Entry und
IRQ-Entry muss deshalb den vollen SSE-State sichern, was aktuell **nicht passiert** --
`sched_preempt` sichert nur bei Preemption, nicht bei jedem IRQ.

---

## 1. Architektur-Abstraktion

### Typ: `arch_fpu_state_t`

Ersetzt das rohe `uint8_t fxsave_area[512]` durch einen opaken, Arch-spezifischen Typ.

```c
/* src/arch/x86_64/fpu.h */
typedef struct {
    uint8_t data[512];    /* Phase 1: FXSAVE */
} __attribute__((aligned(16))) arch_fpu_state_t;

/* src/arch/aarch64/fpu.h */
typedef struct {
    uint64_t vregs[64];   /* V0-V31, je 128 bit = 2x uint64_t */
    uint32_t fpsr;
    uint32_t fpcr;
} __attribute__((aligned(16))) arch_fpu_state_t;
/* Groesse: 64*8 + 4 + 4 = 520 Bytes */
```

### Save-Area-Groessen

| Arch | Feature | Save-Groesse | Alignment | Instruktion |
|------|---------|-------------|-----------|-------------|
| x86_64 | SSE (Baseline) | 512 B | 16 B | FXSAVE/FXRSTOR |
| x86_64 | AVX/AVX2 | 832 B (XSAVE) | 64 B | XSAVE/XRSTOR |
| x86_64 | AVX-512 | 2688 B (XSAVE) | 64 B | XSAVE/XRSTOR |
| ARM64 | NEON (Baseline) | 528 B | 16 B | STP/LDP Q-Regs |
| ARM64 | SVE (128-bit VL) | ~592 B | 16 B | SVE-spezifisch |
| ARM64 | SVE (512-bit VL) | ~2576 B | 16 B | SVE-spezifisch |
| WASM | v128 | 0 B (Kernel) | -- | -- |

XSAVE-Groesse ist dynamisch: `CPUID(0xD, 0).EBX` liefert die exakte Groesse fuer
den aktuellen Feature-Set. Feste Allokation auf Maximum (2688 B) ist fuer einen
RT-Kernel mit statischem Slab akzeptabel.

### API

```c
/* Pro Architektur in src/arch/{x86_64,aarch64}/fpu.c */
void arch_fpu_save(arch_fpu_state_t *state);
void arch_fpu_restore(const arch_fpu_state_t *state);
void arch_fpu_init(arch_fpu_state_t *state);  /* Reset auf CPU-Default */
int  arch_fpu_state_size(void);               /* Runtime: tatsaechliche Groesse */
```

### x86_64: FXSAVE vs XSAVE

Entscheidung bei Boot via CPUID:

| Feature | CPUID-Bit | Save-Methode | XCR0 Bits |
|---------|-----------|-------------|-----------|
| SSE | CPUID.01H:EDX.SSE (bit 25) | FXSAVE | -- |
| XSAVE | CPUID.01H:ECX.XSAVE (bit 26) | XSAVE | -- |
| AVX | CPUID.01H:ECX.AVX (bit 28) | XSAVE | XCR0[2] |
| AVX-512 | CPUID.07H:EBX.AVX512F (bit 16) | XSAVE | XCR0[7:5] |

Logik:
1. Boot: CPUID pruefen. Wenn XSAVE vorhanden → XSAVE-Pfad, XCR0 setzen.
2. Wenn nur SSE → FXSAVE (aktueller Pfad, kein Umbau noetig).
3. Funktionszeiger `arch_fpu_save`/`arch_fpu_restore` einmal bei Boot setzen.

```c
/* Boot-Init (einmalig) */
void arch_fpu_detect(void) {
    uint32_t eax, ebx, ecx, edx;
    __cpuid(1, eax, ebx, ecx, edx);

    if (ecx & (1u << 26)) {        /* XSAVE */
        uint64_t xcr0 = XCR0_X87 | XCR0_SSE;
        if (ecx & (1u << 28))      /* AVX */
            xcr0 |= XCR0_AVX;
        /* CR4.OSXSAVE setzen, dann XCR0 schreiben */
        arch_set_cr4(arch_get_cr4() | CR4_OSXSAVE);
        arch_xsetbv(0, xcr0);
        fpu_use_xsave = 1;
        fpu_xsave_size = __cpuid_ebx(0xD, 0);
    }
}
```

### ARM64: FPSIMD vs SVE

NEON/FPSIMD ist auf ARMv8-A immer vorhanden. Kein Detection noetig.

```c
/* Save: 32 x 128-bit Q-Regs + FPSR + FPCR */
void arch_fpu_save(arch_fpu_state_t *s) {
    __asm__ volatile(
        "stp q0,  q1,  [%0, #0]   \n"
        "stp q2,  q3,  [%0, #32]  \n"
        /* ... alle 32 Register ... */
        "stp q30, q31, [%0, #480] \n"
        "mrs x1, fpsr              \n"
        "mrs x2, fpcr              \n"
        "stp w1, w2, [%0, #512]   \n"
        : : "r"(s) : "x1", "x2", "memory"
    );
}
```

SVE: Registerset ist identisch zu NEON (V0-V31), aber jedes Register ist
`VL` Bits breit (128-2048, in 128er-Schritten). Detection via `mrs x0, ZCR_EL1`.
Speichern mit SVE-spezifischen `STR z0, [base]` etc.

Phase 1 ignoriert SVE. NEON reicht.

### WASM

Kein FPU-State im Kernel. Die WASM-Runtime (`cosmo-wasm-rt`) ist ein normaler
Userspace-Prozess. Dessen SIMD-State (native Register, in die v128 kompiliert wird)
wird durch den normalen Context-Switch-Mechanismus gesichert.

---

## 2. Kernel-SIMD-Policy

### Die Frage

Darf Kernel-Code (memcpy, page_zero, ...) SIMD-Register nutzen?

### Linux-Ansatz

`kernel_fpu_begin()` / `kernel_fpu_end()`:
- Sichert den Userspace-FPU-State in `task_struct->fpu`
- Deaktiviert Preemption (`preempt_disable`)
- Nach `kernel_fpu_end()` wird der State beim naechsten Context-Switch restored

Overhead: 1x XSAVE + 1x XRSTOR pro Nutzung. Bei AVX-512: ~2688 Bytes schreiben/lesen.

### Analyse fuer CosmoRT

**Pro Kernel-SIMD:**
- page_zero mit MOVNTDQ (128-bit non-temporal): 4x Durchsatz vs 8-Byte Stores
- Grosse memcpy: 16 Bytes/Store statt 8 → theoretisch 2x
- page_copy: identisch

**Contra:**
- FXSAVE/FXRSTOR-Overhead: 512 Bytes lesen+schreiben = ~150 Zyklen total
- Bei kleinen Operationen (<256 Bytes) dominiert der Save/Restore-Overhead
- Komplexitaet: kernel_fpu_begin/end muss in jedem Codepfad korrekt sein
- RT-Determinismus: FXSAVE-Latenz ist nicht konstant (haengt von Register-Inhalt ab)

**Kritisches Problem im Ist-Zustand:**
Der Kernel kompiliert ohne `-mno-sse`. Das bedeutet GCC darf SSE nutzen --
unkontrolliert, ueberall. Das ist ein Bug. Zwei Loesungen:

1. `-mno-sse -mno-mmx` global in KCFLAGS, explizite SIMD nur in markierten Funktionen
2. Eager FPU-Save bei jedem Kernel-Entry (teuer, Linux-Weg)

### Empfehlung

**Option 1: `-mno-sse` global + explizite SIMD-Funktionen.**

Begruendung:
- RT-Kernel: Determinismus > Throughput
- page_zero/memcpy sind nicht Hot-Path im RT-Core (der kopiert keine Pages)
- Compute-Cores profitieren, aber der Gewinn rechtfertigt nicht die Komplexitaet
- Einfacher zu auditieren: SIMD ist nur dort, wo es explizit steht

```makefile
# Makefile
KCFLAGS += -mno-sse -mno-mmx -mno-avx
```

Fuer explizite SIMD-Funktionen:

```c
/* __attribute__((target("sse2"))) hebt -mno-sse fuer diese Funktion auf */
__attribute__((target("sse2"), noinline))
static void page_zero_sse2(void *page) {
    /* kernel_fpu_begin() hier */
    __asm__ volatile(
        "pxor %%xmm0, %%xmm0    \n"
        ".rept 256                \n"
        "movntdq %%xmm0, (%0)   \n"
        "add $16, %0             \n"
        ".endr                   \n"
        "sfence                  \n"
        : "+r"(page) : : "xmm0", "memory"
    );
    /* kernel_fpu_end() hier */
}
```

### kernel_fpu_begin/end Implementierung

```c
void kernel_fpu_begin(void) {
    thread_t *t = percpu_self()->current_thread;
    if (t)
        arch_fpu_save(&t->fpu_state);
    /* Kein preempt_disable noetig: CosmoRT preempted nur bei Ring-3-Return */
}

void kernel_fpu_end(void) {
    thread_t *t = percpu_self()->current_thread;
    if (t)
        arch_fpu_restore(&t->fpu_state);
}
```

Vereinfachung gegenueber Linux: CosmoRT preempted den Kernel nicht (Check
`(f[18] & 3) != 3` in `sched_preempt`). Deshalb kein `preempt_disable` noetig.
Der Save ist trotzdem noetig fuer den Fall, dass zwischen kernel_fpu_begin und
kernel_fpu_end ein Interrupt kommt, der einen Kontext-Switch des aktuellen
Threads ausloest -- was aktuell nicht passiert, aber defensiv korrekt.

---

## 3. SIMD-optimierte Kernel-Funktionen

### page_zero (4096 Bytes nullen)

Aktuell: `uint64_t` Stores, 512 Iterationen a 8 Bytes.

| Methode | Bytes/Store | Stores | Bypasses Cache | Latenz (est.) |
|---------|-------------|--------|----------------|---------------|
| Ist (u64) | 8 | 512 | Nein | ~260 Zyklen |
| REP STOSQ (ERMS) | 8 | microcode | Ja (>page) | ~100 Zyklen |
| MOVNTDQ (SSE2) | 16 | 256 | Ja | ~80 Zyklen |
| VMOVNTDQ (AVX) | 32 | 128 | Ja | ~60 Zyklen |

**x86_64 SSE2:**
```c
__attribute__((target("sse2")))
void page_zero_simd(void *page) {
    __m128i zero = _mm_setzero_si128();
    char *p = (char *)page;
    for (int i = 0; i < 4096; i += 64) {
        _mm_stream_si128((__m128i *)(p + i),      zero);
        _mm_stream_si128((__m128i *)(p + i + 16),  zero);
        _mm_stream_si128((__m128i *)(p + i + 32),  zero);
        _mm_stream_si128((__m128i *)(p + i + 48),  zero);
    }
    _mm_sfence();
}
```

**ARM64 NEON:**
```c
void page_zero_simd(void *page) {
    uint8_t *p = (uint8_t *)page;
    /* STNP: Store Non-temporal Pair, umgeht Cache */
    for (int i = 0; i < 4096; i += 64) {
        __asm__ volatile(
            "stnp q0, q0, [%0]     \n"
            "stnp q0, q0, [%0, #32]\n"
            : : "r"(p + i) : "memory"
        );
    }
    __asm__ volatile("dsb st" ::: "memory");
}
/* Voraussetzung: Q0 vorher auf 0 setzen (movi v0.16b, #0) */
```

**Alternative ohne SIMD:** REP STOSQ mit ERMS ist auf modernen Intel-CPUs
vergleichbar schnell und braucht keine Register-Sicherung. Detection ueber
`memops_has_erms` (bereits vorhanden).

```c
void page_zero_erms(void *page) {
    __asm__ volatile(
        "cld          \n"
        "rep stosq    \n"
        : : "D"(page), "c"(512), "a"(0) : "memory"
    );
}
```

Empfehlung: ERMS als Primaer-Optimierung (kein FPU-Save noetig), SIMD nur bei ERMS-Fehlen.

### kmemcpy (grosse Kopien)

Threshold: SIMD nur bei len >= 256 Bytes.

**x86_64 SSE2 (non-temporal, destination nicht im Cache):**
```c
__attribute__((target("sse2")))
void kmemcpy_nt(void *dst, const void *src, size_t len) {
    /* Alignment auf 16-Byte Grenze */
    const __m128i *s = (const __m128i *)src;
    __m128i *d = (__m128i *)dst;
    size_t chunks = len / 64;
    for (size_t i = 0; i < chunks; i++) {
        __m128i a = _mm_loadu_si128(s);
        __m128i b = _mm_loadu_si128(s + 1);
        __m128i c = _mm_loadu_si128(s + 2);
        __m128i d0 = _mm_loadu_si128(s + 3);
        _mm_stream_si128(d,     a);
        _mm_stream_si128(d + 1, b);
        _mm_stream_si128(d + 2, c);
        _mm_stream_si128(d + 3, d0);
        s += 4; d += 4;
    }
    _mm_sfence();
    /* Tail: byte-at-a-time */
}
```

**ARM64 NEON:**
```c
void kmemcpy_simd(void *dst, const void *src, size_t len) {
    /* LDP/STNP Paare: 32 Bytes/Iteration */
    __asm__ volatile(
        "1: ldp q0, q1, [%1], #32  \n"
        "   stnp q0, q1, [%0], #32 \n"  /* STNP: #32 nicht gueltig */
        /* ARM64: STNP hat kein Post-Increment. Manuell: */
        "   stnp q0, q1, [%0]      \n"
        "   add %0, %0, #32        \n"
        "   subs %2, %2, #32       \n"
        "   b.ge 1b                \n"
        : "+r"(dst), "+r"(src), "+r"(len) : : "v0", "v1", "memory"
    );
}
```

Erwarteter Speedup: 2-4x gegenueber word-at-a-time fuer Copies > 1 KB.
Bei < 256 Bytes: FPU Save/Restore dominiert, C-Pfad bleibt schneller.

### kmemset (grosse Fills)

Analog zu page_zero, aber mit beliebigem Byte-Wert.

**x86_64:** `_mm_set1_epi8(val)` + MOVNTDQ. Alternativ REP STOSB (ERMS).

**ARM64:** `dup v0.16b, wN` + STNP-Loop.

REP STOSB mit ERMS ist auf x86_64 oft die beste Wahl — Microcode-optimiert,
kein FPU-Save noetig, linear skalierend.

### page_copy (COW Duplication)

Spezialfall von memcpy mit len=4096, aligned.

```c
void page_copy(void *dst, const void *src) {
    /* Identisch zu kmemcpy, aber ohne Alignment-Check (Pages sind 4K-aligned)
     * und mit fester Laenge → Loop Unrolling moeglich */
}
```

Speedup: identisch zu kmemcpy. 256 SSE-Stores statt 512 Word-Stores.

### Checksum (IP/TCP)

IP/TCP-Checksummen sind 16-bit One's Complement Summen. SIMD beschleunigt durch
parallele Addition breiter Worte.

**x86_64 SSE2:**
```c
__attribute__((target("sse2")))
uint16_t ip_checksum_simd(const void *data, size_t len) {
    __m128i sum = _mm_setzero_si128();
    const __m128i *p = (const __m128i *)data;
    /* 8 x uint16_t parallel addieren */
    while (len >= 16) {
        __m128i v = _mm_loadu_si128(p++);
        sum = _mm_add_epi16(sum, v);  /* Vereinfacht — Carry handling fehlt */
        len -= 16;
    }
    /* Horizontal fold + tail */
}
```

Realistisch: Die Checksumme ist selten im Hot-Path des RT-Cores (Netzwerk-RX
laeuft auf dem RT-Core, aber der Payload wird nicht checksum'd — das macht TCP
im Compute-Thread). Niedrige Prioritaet.

### CosmoFS Block-Hashing

Derzeit nicht implementiert (CosmoFS v1 hat kein Content-Addressing).
Falls CosmoFS v2 kommt: SHA-256 oder BLAKE3 profitieren massiv von SIMD.
BLAKE3 ist explizit fuer SIMD designed (4-way AVX2, 16-way AVX-512).
SHA-256 hat auf x86 SHA-NI Extensions (CPUID.07H:EBX.SHA, bit 29).

Relevanz: erst bei CosmoFS v2. Kein Handlungsbedarf jetzt.

---

## 4. WASM SIMD Integration

### Architektur

```
User:    WASM Binary (\0asm magic)
         │
Kernel:  exec() erkennt \0asm → startet cosmo-wasm-rt als Userspace-Prozess
         │
User:    cosmo-wasm-rt (JIT Compiler)
         ├── v128 ops → SSE (x86_64) / NEON (ARM64)
         └── normaler Userspace-Prozess mit normalem FPU-State
```

### Kernel-Seite

Keine WASM-Awareness noetig. cosmo-wasm-rt ist ein ELF-Binary wie jedes andere.
Der Kernel sichert dessen FPU-State bei Context-Switch. Die JIT-compilierten
v128-Operationen nutzen native SIMD-Register, die der Kernel als opaken
FPU-State behandelt.

### JIT-Mapping

| WASM SIMD | x86_64 SSE | ARM64 NEON |
|-----------|-----------|------------|
| v128.load | MOVDQU | LDR Q |
| v128.store | MOVDQU | STR Q |
| i32x4.add | PADDD | ADD V.4S |
| f32x4.mul | MULPS | FMUL V.4S |
| f32x4.relaxed_madd | VFMADD (FMA) / MULPS+ADDPS | FMLA V.4S |
| i8x16.shuffle | PSHUFB (SSSE3) | TBL V.16B |

### Relaxed SIMD: Non-Determinismus

WASM SIMD "relaxed" Operationen (relaxed_madd, relaxed_swizzle, ...) haben
implementierungsdefinierte Semantik. Verschiedene Hosts liefern verschiedene
Ergebnisse fuer Grenzfaelle (NaN-Propagation, Out-of-Range Lane-Select).

Implikationen fuer CosmoRT:
- **Kein Kernel-Problem.** Die WASM-Runtime definiert das Verhalten.
- **Cross-Platform-Divergenz:** Gleiches WASM-Binary liefert auf x86_64
  andere Ergebnisse als auf ARM64 bei relaxed Ops.
- **Empfehlung:** cosmo-wasm-rt sollte relaxed Ops per Default erlauben
  (Performance), aber ein `--deterministic` Flag anbieten das sie durch
  strikte Varianten ersetzt (fuer Debugging/Replay).

---

## 5. Multi-Arch Build-Strategie

### Compiler-Flags

```makefile
# ── x86_64 ──
KCFLAGS_x86_64 = -mno-sse -mno-mmx -mno-avx -mno-80387
# Kernel-Code: kein SIMD, kein x87. Pure Integer.

# Explizite SIMD-Funktionen: per-function target attribute
# __attribute__((target("sse2")))     → SSE2
# __attribute__((target("avx2")))     → AVX2
# __attribute__((target("avx512f")))  → AVX-512

# ── ARM64 ──
KCFLAGS_aarch64 = -mgeneral-regs-only
# Verhindert NEON/FP-Nutzung im Kernel-Code.
# Equivalent zu -mno-sse auf x86.
# Explizite NEON-Funktionen: Inline-ASM (kein target-attribute auf ARM).

# ── RISC-V 64 (Zukunft) ──
KCFLAGS_riscv64 = -march=rv64gc -mabi=lp64d
# Kein Vektor-Extension im Kernel.
```

### Runtime-Detection (x86_64)

```c
/* Boot: einmal pruefen, Funktionszeiger setzen */
static void (*page_zero_fn)(void *);
static void (*kmemcpy_fn)(void *, const void *, size_t);

void memops_init(void) {
    /* ... CPUID ... */
    if (memops_has_erms)
        page_zero_fn = page_zero_erms;    /* REP STOSQ, kein SIMD noetig */
    else
        page_zero_fn = page_zero_generic; /* C word-at-a-time */

    /* SIMD-Varianten nur bei explizitem kernel_fpu_begin/end */
}
```

ARM64: Kein Runtime-Detection fuer NEON (immer vorhanden). SVE-Detection
via `mrs x0, ID_AA64PFR0_EL1` (SVE-Bit [35:32]).

### Verzeichnisstruktur

```
src/arch/x86_64/
    fpu.c           FXSAVE/XSAVE, kernel_fpu_begin/end
    simd_memops.c   page_zero_sse2, kmemcpy_nt  (mit target-Attributen)
src/arch/aarch64/
    fpu.c           FPSIMD Save/Restore
    simd_memops.c   NEON page_zero, memcpy
```

---

## 6. Context-Switch SIMD-State

### Eager vs Lazy

**Lazy (historisch, Linux < 4.2):**
- FPU-State wird nicht bei jedem Switch gesaved
- CR0.TS-Bit gesetzt → erste FPU-Nutzung loest #NM Exception aus
- Exception-Handler: save alter State, restore neuer State, clear TS
- Vorteil: Spart Save/Restore wenn Thread kein FPU nutzt
- Nachteil: Timing-Seitenkanal, #NM-Latenz, Spectre-Variante

**Eager (Linux >= 4.2, 2016):**
- FPU-State bei jedem Context-Switch gesaved/restored
- Deterministisch, kein #NM, kein Seitenkanal
- Nachteil: immer 512-2688 Bytes kopieren

**Linux-History:** Commit `58122bf1d856` (2016) switchte auf Eager wegen
Lazy-FPU-Leak (CVE-2018-3665 kam spaeter und bestaetigte die Entscheidung).

### Empfehlung fuer CosmoRT

**Eager.** Bereits implementiert (FXSAVE in preempt, FXRSTOR in thread_run).

Begruendung:
- RT-Kernel: deterministische Latenz > durchschnittliche Latenz
- Lazy spart im Mittel, hat aber Worst-Case-Spikes (#NM Handler)
- Nahezu alle Userspace-Prozesse nutzen FPU (libc, printf, etc.)
- Sicherheit: Lazy FPU Leak ist ein bekannter Angriffsvektor

### State-Groessen pro Feature-Level

| Arch | Feature | State-Groesse | thread_t Impact |
|------|---------|--------------|-----------------|
| x86_64 | x87 + SSE (FXSAVE) | 512 B | +512 B (aktuell) |
| x86_64 | + AVX (XSAVE) | 832 B | +832 B |
| x86_64 | + AVX-512 (XSAVE) | 2688 B | +2688 B |
| ARM64 | NEON/FPSIMD | 528 B | +528 B |
| ARM64 | + SVE (256-bit VL) | ~1040 B | +1040 B |
| ARM64 | + SVE (512-bit VL) | ~2576 B | +2576 B |

### Impact auf thread_t und Slab

Aktuell: `thread_t` mit `fxsave_area[512]` am Ende ≈ 1100-1200 Bytes.
Slab-Allokation: 2048-Byte Slab Slot (naechste Power-of-2).

Bei XSAVE (AVX-512): +2688 statt +512 = thread_t ≈ 3400 Bytes → 4096-Byte Slab.
Pro Thread 1 Page. Bei THREAD_MAX=64: 64 Pages = 256 KB. Akzeptabel.

**Optimierung:** `arch_fpu_state_t` separat allozieren (nicht inline in thread_t).
Dann bleibt thread_t kompakt, und der FPU-State wird separat Slab-alloziert
mit korrektem Alignment.

```c
typedef struct thread {
    /* ... wie bisher, ohne fxsave_area ... */
    arch_fpu_state_t *fpu;  /* Zeiger statt inline */
} thread_t;
```

Vorteil: thread_t bleibt ~700 Bytes. FPU-State hat eigenen Slab mit
64-Byte Alignment (XSAVE-Anforderung). Nachteil: eine Indirektion.
Bei Eager Save/Restore ist der FPU-State aber nur 2x pro Switch
zugegriffen — die Indirektion ist irrelevant.

---

## 7. Prioritaeten

### Phase 1 (jetzt)

1. **`-mno-sse -mno-mmx` in KCFLAGS** — Bug fixen. Der Kernel darf SSE nicht
   implizit nutzen, solange kein Save bei jedem Kernel-Entry stattfindet.

2. **REP STOSQ fuer page_zero** — ERMS ist bereits detected (`memops_has_erms`).
   Kein FPU-Save noetig, kein SIMD. Einfachster Speedup.

3. **REP MOVSQ fuer kmemcpy** — Analog. ERMS-optimiert auf Intel seit Ivy Bridge.

4. **`arch_fpu_state_t`-Abstraktion** — Typ einfuehren, `fxsave_area` ersetzen.
   Vorbereitung fuer Multi-Arch. Kein Funktionalitaetswechsel.

### Phase 2 (ARM64 Port)

5. **ARM64 FPSIMD Save/Restore** — 32x Q-Register + FPSR/FPCR.
   Muss vor dem ersten ARM64 Context-Switch fertig sein.

6. **ARM64 page_zero/memcpy mit NEON** — STNP-basiert, kein Cache-Pollution.
   Aber: erst `-mgeneral-regs-only` im Kernel, dann explizite NEON-Funktionen.

7. **XSAVE-Pfad fuer x86_64** — Wenn AVX/AVX-512-Userspace-Binaries laufen sollen.
   Ohne XSAVE werden AVX-Register bei Context-Switch nicht gesichert → Corruption.

### Phase 3 (Optimierung)

8. **SSE2 page_zero/page_copy** — Nur wenn ERMS nicht verfuegbar (alte CPUs, AMD
   vor Zen). Mit `kernel_fpu_begin/end`.

9. **SIMD-Checksum** — Nur wenn Netzwerk-Throughput zum Bottleneck wird.

10. **SVE-Support** — Nur wenn ARM64-Hardware mit SVE-Anforderungen auftaucht.

### Was wir NICHT machen

- **AVX-512 im Kernel.** Nicht die 2688-Byte Save-Area und die
  Frequency-Throttling-Probleme in den Kernel holen. Userspace darf AVX-512
  nutzen (der Kernel sichert den State), aber Kernel-Code bleibt bei SSE2 max.

- **Lazy FPU.** Kein CR0.TS, kein #NM. Eager ist einfacher, sicherer,
  deterministischer.

- **SIMD in IRQ-Handlern.** Der RT-Core verarbeitet IRQs. SIMD im IRQ
  wuerde kernel_fpu_begin/end in jedem IRQ-Handler erfordern. Nicht akzeptabel.

- **Runtime CPU-Dispatch fuer jede memcpy-Groesse.** Ein Funktionszeiger pro
  Funktion, gesetzt bei Boot. Keine if-Kaskaden im Hot-Path.

- **WASM-SIMD-Awareness im Kernel.** Die WASM-Runtime ist Userspace. Der Kernel
  behandelt sie wie jeden anderen Prozess.
