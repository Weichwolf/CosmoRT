# Timer-Treiber Design — ACPI, virtio, Hyper-V

Recherche. Kein Code. Commit `68bca80`, Branch `ltp`.

## 0. Ist-Zustand

| Komponente                        | Datei                                       | Stand |
|-----------------------------------|---------------------------------------------|-------|
| TSC-Calibration via PIT (~10ms)   | `src/arch/x86_64/timer/timer.c`             | ok, blockiert Boot |
| TSC monotonic                     | `src/kernel/core/hrtimer.c::hrtimer_now_ns` | ok, **kein Invariant-Check** |
| LAPIC one-shot clockevent         | `src/kernel/core/hrtimer.c`                 | ok |
| CMOS-RTC (wall-epoch)             | `src/arch/x86_64/timer/timer.c::rtc_init`   | ok |
| HAL-Timer API                     | `include/kernel/hal/hal_timer.h`            | minimal: now_ns/set_oneshot/set_periodic/disarm |
| HPET                              | —                                           | fehlt |
| ACPI-Parser (RSDP→XSDT→FADT/HPET) | `src/boot/boot.c` (RSDP nur als Entropy)    | fehlt |
| ACPI PM_TMR                       | —                                           | fehlt |
| Hyper-V Reference TSC             | `src/arch/x86_64/hw/hyperv.c::hyperv_tsc_time_ns` | existiert, **ungenutzt** im hrtimer-Pfad |
| Hyper-V STimer                    | nur MSR-Defines in `include/kernel/hw/hyperv.h` | fehlt |
| virtio-rtc                        | —                                           | fehlt |
| KVM pvclock                       | —                                           | fehlt |
| `struct clocksource` Abstraktion  | —                                           | fehlt |

Kernbefund: Hyper-V-Infrastruktur (Hypercall, SynIC, TSC-Page) ist vorhanden aber
wird im Zeit-Pfad nicht konsumiert. `hrtimer` nimmt direkt `rdtsc` + LAPIC-Timer,
ohne Erkennung ob TSC unter Hyper-V/KVM zuverlässig ist. Konfidenz: hoch (Grep
ausgeführt).

---

## 1. Voraussetzung: clocksource/clock_event Core

Ohne Abstraktion wird jeder neue Treiber `#ifdef`-Gewurstel in `hrtimer.c`.

```
struct clocksource { const char *name; int rating; uint64_t (*read)(void);
                     uint64_t mask, mult, shift; uint32_t flags; };
struct clock_event_device { const char *name; int rating; uint32_t features;
                            int (*set_next_event)(uint64_t delta_ns); ... };
int clocksource_register(struct clocksource *);
uint64_t clocksource_read_ns(void);
```

Location: `src/kernel/core/clocksource.{c,h}`.

HAL-API bleibt **unverändert** (aarch64-generic). Intern:
`hal_timer_now_ns` → `clocksource_read_ns()`, `hal_timer_set_oneshot` →
`clock_event->set_next_event()`. x86_64 registriert mehrere Quellen, aarch64
registriert CNTVCT_EL0 als einzige.

Aufwand: ~350 LOC, 2 Tage.

---

## 2. ACPI: HPET + PM_TMR

### Linux-Vorbild
- `drivers/acpi/tables.c` — RSDP→XSDT→Signatur-Suche
- `arch/x86/kernel/hpet.c` — clocksource rating 250 + clock_event pro Comparator
- `drivers/clocksource/acpi_pm.c` — rating 110, 3.579545 MHz

### CosmoRT-Plan

**ACPI-Parser** (`src/kernel/acpi/{acpi,tables}.c`, ~400 LOC). Minimalumfang:

| Tabelle | Zweck                                       |
|---------|---------------------------------------------|
| RSDP    | bereits aus EFI via `boot_info.rsdp_addr`   |
| XSDT    | Verzeichnis                                 |
| FADT    | `pm_tmr_blk`, `pm_tmr_len`, TSC-deadline    |
| HPET    | Basisadresse, Block-ID, min_tick            |
| MADT    | LAPIC/IOAPIC (später für SMP)               |

Keine ACPICA-Integration (~500 kLOC) — reines Table-Parsing (Signatur, Checksum,
Struct-Layout) reicht.

**HPET** (`src/arch/x86_64/timer/hpet.c`, ~300 LOC):
MMIO aus ACPI-HPET mappen, Counter-Clock-Period lesen → mult/shift ableiten,
als clocksource rating 250 registrieren. Optional ein Comparator als
clock_event rating 150 (LAPIC bleibt Default).

**PM_TMR** (`src/arch/x86_64/timer/acpi_pm.c`, ~120 LOC):
I/O-Port aus FADT, 24/32-bit Counter (Flag `TMR_VAL_EXT`), feste 3.579545 MHz,
rating 110 — reiner Sanity-Fallback.

### Init-Reihenfolge
```
kmain → acpi_init(rsdp) → clocksource_init → hpet_init? → acpi_pm_init?
     → clocksource_select → hal_timer_init (LAPIC via HPET kalibriert)
```

TSC-Kalibration via HPET statt PIT spart die 10-ms Boot-Blockade und ist genauer.

### Fallen
- HPET 32 vs. 64-bit Counter: General Cap Bit 13 prüfen, sonst Wrap-Accounting.
- PM_TMR 24-bit Wrap alle ~4.6 s — Lese-Pfad muss Wrap handhaben.
- **TSC-Invariant** (`CPUID.80000007H:EDX[8]`) muss geprüft werden bevor TSC
  mit `CONTINUOUS` registriert wird. Aktuell fehlt.

Aufwand ACPI: **4 Tage gesamt** (Parser 2 + HPET 1.5 + PM_TMR 0.5).

---

## 3. KVM pvclock + virtio-rtc

### Linux-Vorbild
- `arch/x86/kernel/kvmclock.c` — pvclock-Struct (`wall_clock`, `system_time`)
  in Guest-Memory via MSRs, rating 400 (über HPET). TSC-basiert, kein VMEXIT.
- `drivers/rtc/rtc-virtio.c` — virtio-rtc (spec 1.3, device-ID 11), Clock-Read
  via virtqueue.

### CosmoRT-Plan

**KVM pvclock zuerst** — QEMU/KVM ist Hauptziel der `make qemu-*`-Tests.

Location: `src/arch/x86_64/hw/kvmclock.c`.
- Detection: `CPUID.40000000H` = `"KVMKVMKVM\0\0\0"`, Feature-Bit `40000001H:EAX`
- 2 Pages DMA (analog Hyper-V TSC-Page)
- MSR: `KVM_SYSTEM_TIME_NEW` (0x4b564d01), `KVM_WALL_CLOCK_NEW`
- Read-Loop mit Version-Counter (Bit 0 = Update in Progress)
- clocksource rating 400

Aufwand: ~200 LOC, 1 Tag.

**virtio-rtc** zweitrangig. QEMU erst ab ~8.2, unter KVM nimmt man kvmclock.
Relevant nur für virtio-only Hosts (cloud-hypervisor minimal). Location später:
`src/drivers/virtio/virtio_rtc.c`. Aufwand: ~300 LOC, 1.5 Tage.

### Fallen
- pvclock-TSC-Scale nicht stable über VM-Migration → Re-Kalibrierung nach
  Suspend/Resume nötig (aktuell kein Suspend in CosmoRT, nur merken).
- `system_time` läuft bei Host-Suspend nicht weiter.

---

## 4. Hyper-V synthetic timers

### Linux-Vorbild (`drivers/clocksource/hyperv_timer.c`)
Zwei separate Komponenten:
1. **Reference TSC Page** → clocksource rating 250–400 (abhängig von TSC-Invariant)
2. **STimer** → clock_event pro vCPU, 4 Timer (STIMER0..3) via MSR
   `STIMER0_CONFIG/COUNT`. Rating 300 (über LAPIC). Expiry via SynIC-SINT
   (Direct Mode, präferiert) oder VMBus-Message (Indirect).

### CosmoRT-Stand
- TSC-Page **gemappt und lesbar** (`hyperv_tsc_time_ns`), aber nie als
  clocksource registriert → `hrtimer_now_ns` nutzt roh-TSC auch unter Hyper-V.
  Echter Bug mit nested TSC-Scaling oder Migration.
- STimer: nur MSR-Defines, keine Logik.
- SynIC: funktioniert für VMBus (SINT2/3). Direct-Mode STimer braucht weiteren
  SINT (z.B. SINT4 → Vektor 0x33).

### Plan

Location: `src/arch/x86_64/timer/hyperv_timer.c` (arch-spezifisch wegen MSR).

```
hyperv_timer_init():
  if hyperv_detect() && tsc_page_valid:
    register clocksource "hyperv_tsc" rating=400          // ~60 LOC, 0.3 Tage
  if CPUID.40000003H:EAX bit 19 (SynticDirectMode):
    allocate SINT4 → vector 0x33 → stimer_isr
    register clock_event "hv_stimer0" rating=300          // ~330 LOC, 2 Tage
  else: fallback LAPIC
```

Capability-Bits (Konfidenz mittel, vor Impl gegen TLFS v6.0b prüfen):
`40000003H:EAX` Bit 1 (SynIC MSRs), Bit 4 (Synthetic Timers), Bit 19 (Direct Mode).

### Fallen
- STimer-COUNT ist **absolute** 100ns-Zeit seit Boot, kein Delta. Für
  `set_next_event(delta_ns)` → `tsc_page_time + delta/100`.
- STimer-CONFIG Auto-Enable-Bit setzen — ein verpasster Config-Write nach
  Message-Delivery macht den Timer sonst stumm.
- Bei Migration kann TSC-Scale neu ausgehandelt werden; die TSC-Page sequence
  counter fängt das ab.

---

## 5. Priorität

| # | Task                         | Host-Häufigkeit       | Aufwand | Value | Begründung |
|---|------------------------------|-----------------------|---------|-------|------------|
| 1 | clocksource/clock_event Core | — (Voraussetzung)     | 2 d     | hoch  | Ohne Abstraktion Hack |
| 2 | ACPI Table-Parser            | immer (bare-metal+VM) | 2 d     | hoch  | Voraussetzung für HPET + MADT/SMP |
| 3 | HPET                         | bare-metal + QEMU def.| 1.5 d   | hoch  | Ersetzt 10-ms-PIT-Kalib, präziser |
| 4 | Hyper-V TSC clocksource-Wrap | WSL2, Azure           | 0.3 d   | mittel| Trivial, fixt latenten Bug |
| 5 | KVM pvclock                  | QEMU/KVM Tests        | 1 d     | mittel| Bessere Zeit unter `make qemu-*` |
| 6 | Hyper-V STimer               | WSL2, Azure           | 2 d     | mittel| Relevant sobald CosmoRT in WSL |
| 7 | ACPI PM_TMR                  | Sanity-Fallback       | 0.5 d   | niedrig| Nur ohne HPET |
| 8 | virtio-rtc                   | minimal-Hypervisor    | 1.5 d   | niedrig| Nischig, kvmclock deckt 95% |

**Begründung**:
- (1)+(2) sind Infrastruktur, alles andere hängt daran.
- (3) hat höchsten ROI — Boot-Beschleunigung + Präzision.
- (4) ist fast gratis und fixt echten Bug.
- (5)+(6) sind Komfort für VM-Hosts.
- (7)+(8) sind Vollständigkeit.

Konfidenz Aufwand: mittel (±30%). CPUID-Bits und MSR-Nummern im Report aus
Gedächtnis — vor Implementierung gegen aktuelle Specs (ACPI 6.5, Hyper-V TLFS
v6.0b, KVM API-docs) verifizieren.

---

## 6. Offene Fragen

1. TSC-Invariant-Check nachrüsten im Zuge von (1)?
2. aarch64: CNTVCT_EL0 als Default-clocksource registrieren — separater Task
   oder Teil des Core-Patches?
3. Welche LTP-Tests prüfen Clock-Semantik (`clock_gettime`-Monotonie,
   `clock_nanosleep`-Präzision)? Die müssen nach jedem Timer-Patch grün bleiben.
