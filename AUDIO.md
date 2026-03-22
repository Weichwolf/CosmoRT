# CosmoOS Audio

12-Spur Mixer im Kernel. RT-safe, <1ms Latenz.
Jeder VT-Slot = eine Audio-Spur.

## Mixer

```
Spur 1 ──[vol]──[pan]──┬──→ Mix-Bus (dry)
                        └──[send]──→ FX-Bus (wet)

Spur 2 ──[vol]──[pan]──┬──→ Mix-Bus
                        └──[send]──→ FX-Bus
...
Spur 12 ─[vol]──[pan]──┬──→ Mix-Bus
                        └──[send]──→ FX-Bus

FX-Bus ──[Master FX]──→ Mix-Bus

Mix-Bus ──[Master Compressor]──→ DAC
```

## Pro Spur

| Parameter | Range | Default |
|-----------|-------|---------|
| Volume    | 0-100 | 100     |
| Pan       | L100..R100 | Center |
| FX Send   | 0-100 | 0       |
| Mute/Pin  | stumm / aktiv / pinned | stumm |

```
cosmo$ audio vol 80           # dieser Slot
cosmo$ audio pan L30          # leicht links
cosmo$ audio send 40          # 40% zum FX-Bus
cosmo$ audio pin              # spielt im Hintergrund
```

## Fokus-Verhalten

```
● aktiv    fokussierter Slot, Audio spielt automatisch
○ stumm    Hintergrund-Slot, kein Audio (Default)
♪ pinned   spielt immer, auch im Hintergrund
```

Fokus-Wechsel: aktiver Slot wechselt, pinned Slots spielen weiter.
Mixer ueberspringt stumme Spuren.

## Master FX

Ein Effekt-Slot (Reverb, Delay, Chorus — umschaltbar).
FX-Send pro Spur bestimmt wieviel beigemischt wird.
Doom 100% dry, Notifications 30% Hall.

## Master Compressor

Immer an. Schuetzt die Ohren. Begrenzt Peaks.
Konfiguration: Threshold, Ratio, Attack, Release.
Sinnvolle Defaults, muss man nie anfassen.

## Plugin-API (fuer DAW-Entwickler)

Der OS-Mixer ist bewusst einfach (1 FX, 1 Comp, 12 Spuren).
Wer mehr will baut eine DAW als App auf einem Slot.

Plugins in C, RT-safe:

```c
void plugin_process(audio_buf_t *buf);
void plugin_param_set(int id, float value);
float plugin_param_get(int id);
```

GUI-Widgets (Kernel rendert, Ableton-Stil):

```c
void plugin_ui(ui_ctx_t *ctx) {
    ui_knob(ctx, "Decay", &params.decay, 0.1, 10.0);
    ui_knob(ctx, "Mix",   &params.mix,   0.0, 1.0);
    ui_slider(ctx, "Pre-Delay", &params.predelay, 0, 200);
    ui_toggle(ctx, "Bypass", &params.bypass);
    ui_meter(ctx, "Level", level_db);
}
```

Knob, Slider, Toggle, Meter, Dropdown. Reicht fuer jeden Plugin.

## Kernel vs Userspace

```
Kernel (RT-Thread, <1ms):
  12-Spur Summe, Volume, Pan, FX-Send-Routing
  Master Compressor (immer an)

Userspace (audiod):
  Master FX (Plugin, umschaltbar)
  Plugin-Management
  Konfiguration

App (DAW auf eigenem Slot):
  Beliebig viele FX pro Kanal
  Eigenes Routing, eigene Plugins
  Liefert fertigen Stereo-Mix an seine Spur
```

## Hardware

- 48kHz, 10ms Buffer (480 Samples)
- Audio-Callback: ~0.1ms Arbeit pro Callback = 1% CPU
- RT-Thread: SCHED_FIFO, hoechste Prioritaet, preemptiv
- Kein Stottern, egal was auf den POSIX-Cores passiert
