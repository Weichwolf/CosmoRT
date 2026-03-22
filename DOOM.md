# Doom auf CosmoOS

Doom als WASM im CosmoUI-Browser. Kein Portierungsaufwand —
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
