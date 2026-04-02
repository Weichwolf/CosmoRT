# WASM auf CosmoOS

WASM ist ein natives Binary-Format. Ausfuehrbar wie ELF.

```
cosmo$ doom.wasm
cosmo$ ffmpeg.wasm input.mp4 output.webm
cosmo$ sqlite.wasm mydb.db "SELECT * FROM t"
```

## WASM als First-Class Binary

Kernel erkennt WASM am Magic (`\0asm`) und laedt die Runtime:

```c
// ELF-Loader in elf.c
if (magic == "\x7fELF")  → ELF-Loader
if (magic == "\0asm")     → exec /usr/lib/cosmo-wasm-rt <datei>
```

Der Kernel hat keinen WASM-Loader. Er liest 4 Bytes Magic,
erkennt `\0asm`, und macht execve auf cosmo-wasm-rt. Dreizeiler
in elf.c — wie ein Shebang auf Binary-Ebene.

Die gesamte WASM-Maschinerie (Parsing, JIT, Ausfuehrung,
WebGL/Audio-Binding) sitzt in CosmoUI. Der Kernel weiss nicht
was WASM ist.

## Zwei WASM-Welten

| Typ | Imports | Runtime | Beispiel |
|-----|---------|---------|----------|
| WASI | `wasi_snapshot_preview1` (fd_read, fd_write, ...) | Thin Layer → POSIX Syscalls | sqlite, ffmpeg, CLI-Tools |
| Emscripten | `env` (emscripten_*, SDL, WebGL) | CosmoUI JS-Engine + Glue | Doom, Spiele, GUI-Apps |

Erkennung beim Laden: WASI importiert aus `wasi_snapshot_preview1`,
Emscripten importiert aus `env` mit Emscripten-spezifischen Namen.
cosmo-wasm-rt waehlt den Pfad automatisch.

WASI ist trivial — Imports mappen fast 1:1 auf CosmoRT-Syscalls.
Emscripten braucht die volle JS-Engine + WebGL/Audio-Bindings.

```
cosmo$ sqlite.wasm mydb.db          ← WASI, kein JS, fast native
cosmo$ doom.wasm                     ← Emscripten, CosmoUI JS-Engine
```

## Doom als Integrationstest

Doom als WASM (Emscripten-kompiliert, existiert). Kein Portierungsaufwand —
die WASM-Binary existiert, die Web-APIs sind spezifiziert.

## Warum

Ein Programm testet den gesamten Stack:
- WASM-Runtime + JS-Engine (CosmoUI)
- WebGL → GPU Command Buffer → Kernel
- Audio RT-Path (PCM-Buffer, 10ms Deadline)
- Input-Latenz (Keyboard/Mouse → Event < 1ms)
- VSync (Kernel-garantiert, kein Tearing)
- File I/O (WAD laden via fetch → CosmoFS)
- Scheduling (Audio-RT preemtet Rendering)

Gleichzeitig Claude Code auf einem anderen Terminal →
SMP-Stresstest unter realer Last.

## Stack

```
doom.wasm (Emscripten-kompiliert, existiert)
  │
  ├── WebGL           → CosmoUI GPU Backend → cosmo_gpu_submit()
  ├── Web Audio       → CosmoUI Audio       → cosmo_audio_submit()  (RT)
  ├── Keyboard/Mouse  → CosmoUI Input       → /dev/input
  ├── fetch (WAD)     → CosmoUI Network     → CosmoFS
  └── requestAnimationFrame → VSync          → cosmo_surface_present()
```

## VSync

Kernel-garantiert. Kein Opt-out. Present blockiert bis VBlank.
Variable Refresh (FreeSync/G-Sync) wenn Hardware es kann.

Kein Tearing, nie, egal was die App macht. Kein
Double-Buffering-Management in der App noetig.

## Validierung

```
F1: cosmo$ claude -p "train a small NN"    ← alle Cores, AVX2
F2: Doom WASM im CosmoUI-Browser           ← 60fps, Audio, Input
```

Bestanden wenn:
- Doom: 60fps ohne Drops, Audio ohne Stottern, Input ohne Lag
- Claude: Training laeuft durch, API-Calls funktionieren
- Beides gleichzeitig stabil
