# CosmoOS Desktop

BeOS-Philosophie: ein Personal Computer ist persoenlich.
Kein Login-Screen, kein Window-Manager, kein Desktop-Environment.

## Boot

12 Fullscreen-Slots auf F1-F12. Sofort bereit.

```
Boot → 12 VTs
F1: cosmo$
F2: cosmo$
...
F12: cosmo$
```

Kein Setup, kein Splash-Screen. Sofort 12 Shells.

## Eine App pro Slot

Jeder Slot ist Fullscreen. Kein Fensterrahmen, kein Resize, kein Drag.
F-Tasten wechseln sofort. Kein Alt+Tab, kein Popup-Picker.

```
F1: [Claude Code]     Terminal
F2: [make]            Terminal
F3: [github.com]      Browser
F4: [Teams]           Browser
F5: [Figma]           WASM-App
F6: [Doom]            WASM-App
F7-F12:               frei
```

12 Slots erzwingen Fokus. Kein Tab-Hoarding, kein 47-Tabs-Browser.
Lesezeichen sind Lesezeichen. Offene Apps sind offene Apps.

## Titel

Apps setzen den Slot-Titel ueber ANSI-Escape (`\033]0;title\007`).
Jede App kennt das schon — Terminals, Browser (<title>), WASM-Apps.

## Bar

Immer sichtbar am unteren Bildschirmrand.

```
┌─────────────────────────────────────────────────┐
│                                                 │
│              Fullscreen App                     │
│                                                 │
├──────────────────────────────┬──────────────────┤
│ [1] 2  3  4  5  6  7  8  9  │  vol net  15:42  │
└──────────────────────────────┴──────────────────┘
  Slot-Indikator (links)        System-Tray (rechts)
```

**Links:** Slot 1-12, aktiver hervorgehoben. Klickbar. Zeigt Titel.

**Rechts:** Uhr, Netzwerk, Audio, Batterie. Fixe Icons.
Nur Kernel/System-Daemons setzen Icons. Kein Drittanbieter-Chaos.

## Fullscreen-Exklusiv

Apps und Games koennen Bar ausblenden. Im Exklusiv-Modus:

- Bar verschwindet — App hat den ganzen Bildschirm
- System-Cursor verschwindet — App zeichnet alles selbst
- F-Tasten wechseln trotzdem den VT (immer, nicht abschaltbar)

## Maus

System-Cursor vom Kernel (Hardware-Cursor). Immer sichtbar
im normalen Modus. Weg im Fullscreen-Exklusiv.

## Terminal ist Default

Jeder Slot startet als Terminal. Apps werden vom Terminal gestartet:

```
cosmo$ browser github.com       # dieser Slot wird Browser
cosmo$ doom.wasm                 # dieser Slot wird Doom
```

Wenn die App schliesst, kommt das Terminal zurueck.

## Browser hat keine Tabs

Eine Webseite pro Slot. Wenn du eine fuenfte Webseite oeffnest,
musst du entscheiden welche der vier weicht. Das ist kein Nachteil.
Du hast nie 47 Tabs gleichzeitig benutzt.

## Audio: 12-Spur Mixer im Kernel

Jeder Slot hat eine eigene Audio-Spur. Kernel mixt im RT-Callback.

```
● aktiv    Audio spielt (fokussierter Slot, automatisch)
○ stumm    kein Audio (Hintergrund-Slots, Default)
♪ pinned   spielt immer, auch im Hintergrund
```

```
cosmo$ audio pin          # dieser Slot spielt immer
cosmo$ audio unpin        # zurueck zu Default
```

```
F1: [Claude Code]  ○ stumm
F2: [make]         ○ stumm
F3: [Spotify]      ♪ pinned    ← spielt immer
F4: [Teams]        ♪ pinned    ← Notifications hoerbar
F5: [Figma]        ○ stumm
F6: [Doom]         ● aktiv     ← hat gerade Fokus
```

Fokus-Wechsel: aktiver Slot wechselt, pinned Slots spielen weiter.
Mixer ueberspringt stumme Spuren — im Normalfall 1-3 statt 12.

## Kein Cruft

- Kein Window-Manager
- Kein Dock
- Kein Minimize/Maximize
- Kein Fensterrahmen
- Kein Alt+Tab
- Kein Wallpaper
- Kein Theme-Engine
- Kein Compositor
- Keine Browser-Tabs

Der Desktop ist 12 Fullscreen-Slots und eine Bar. Nicht mehr.
