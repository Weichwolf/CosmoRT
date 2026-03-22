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

cosmo-wasm-rt ist die CosmoUI WASM-Runtime als Userspace-Binary.
Hat Zugriff auf WebGL/Audio/Input. Fuer den User transparent —
.wasm ist einfach ein ausfuehrbares Programm.

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
