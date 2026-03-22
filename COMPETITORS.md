# CosmoOS — Verwandte Projekte

Stand: 2026-03-22. Nur aktive Projekte (Commits seit 2024).

## Naechste Verwandte

| Projekt | Sprache | Stars | Aehnlichkeit | Fehlt |
|---------|---------|-------|-------------|-------|
| [Redox OS](https://github.com/redox-os/redox) | Rust | 16K | Alles from scratch, COW-FS, eigene libc | kein RT, kein WASM |
| [k23](https://github.com/JonasKruckenberg/k23) | Rust | 623 | WASM als Binary-Format im Kernel | kein POSIX, frueh |
| [Phoenix-RTOS](https://github.com/phoenix-rtos/phoenix-rtos-kernel) | C | 156 | Microkernel + POSIX + RT + C | IoT, kein Desktop |
| [Managarm](https://github.com/managarm/managarm) | C++ | 1.9K | Microkernel + POSIX, async I/O | kein RT, kein WASM |
| [Haiku](https://github.com/haiku/haiku) | C/C++ | 2.2K | BeOS-Philosophie, Single-User | Hybrid-Kernel, Legacy |

## Referenz-Architekturen

| Projekt | Sprache | Stars | Relevant fuer |
|---------|---------|-------|---------------|
| [seL4](https://github.com/seL4/seL4) | C | 5.4K | Capability-Microkernel (CosmoRT IPC basiert darauf) |
| [HelenOS](https://github.com/HelenOS/helenos) | C | 1.5K | Sauberste Multiserver-Microkernel-Implementierung |
| [NuttX](https://github.com/apache/nuttx) | C | 3.8K | POSIX RT Extensions (timer, mq, sem) |
| [NeptuneOS](https://github.com/cl91/NeptuneOS) | C | 427 | OS-Personality auf seL4-Microkernel |

## From-Scratch POSIX (Einzelentwickler)

| Projekt | Stars | Was sie zeigen |
|---------|-------|----------------|
| [ToaruOS](https://github.com/klange/toaruos) | 6.7K | ~100K LOC fuer komplettes OS mit GUI |
| [Dennix](https://github.com/dennis95/dennix) | 162 | Self-Hosting POSIX, eigene libc+shell+coreutils |
| [Sortix](https://gitlab.com/sortix/sortix) | — | Betreibt eigene Infrastruktur auf eigenem OS |
| [Tilck](https://github.com/vvaltchev/tilck) | 3.1K | Welche 100 Syscalls reichen wirklich? |

## Sonstiges

| Projekt | Stars | Einschaetzung |
|---------|-------|---------------|
| [Asterinas](https://github.com/asterinas/asterinas) | 4.4K | Framekernel (Rust), Linux-Ersatz. Anderes Ziel. |
| [Vinix](https://github.com/vlang/vinix) | 2.1K | OS in V. Experimentell. |
| [AIOS](https://github.com/agiresearch/AIOS) | 5.4K | "OS fuer AI Agents" — Python-Framework, kein echtes OS |
| [Xous](https://github.com/betrusted-io/xous-core) | 892 | Microkernel fuer eigene RISC-V Hardware (Precursor) |

## CosmoOS Alleinstellungsmerkmale

Kein aktives Projekt kombiniert alle diese Eigenschaften:

1. **RT + POSIX + Microkernel + C** (Phoenix-RTOS nah, aber IoT)
2. **WASM als natives Binary-Format** (k23 nah, aber Rust + JIT im Kernel)
3. **AI Agent als Zielworkload** (kein anderes Bare-Metal-OS)
4. **COW-FS mit S3 Cloud-Sync** (Redox hat COW-FS, aber ohne Cloud)
5. **Single-User + 12-Slot Desktop + Kernel Audio Mixer**
6. **Eigene libc + Shell + Coreutils + JS-Engine** (Redox aehnlich, in Rust)

## Quellen

- [WASM I/O 2025: Smarter OSes Will Use Wasm](https://2025.wasm.io/sessions/smarter-operating-systems-will-use-wasm-the-coming-os-revolution/)
- [Redox OS Roadmap 2025-2026](https://portallinuxferramentas.blogspot.com/2025/09/redox-os-unveils-ambitious-2025-2026.html)
- [The Register: Three alternative microkernels (2025)](https://www.theregister.com/2025/09/12/three_new_microkernels/)
