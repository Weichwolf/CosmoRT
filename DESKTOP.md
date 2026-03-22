# CosmoOS Desktop

BeOS-Philosophie: ein Personal Computer ist persoenlich.
Kein Login-Screen, kein Desktop-Environment das hochfahren muss.

## Boot

12 Terminals auf F1-F12. Sofort bereit.

```
Boot → 12 VTs mit je einem Terminal
F1: cosmo$
F2: cosmo$
...
F12: cosmo$
```

Kein Setup, kein Splash-Screen. Sofort 12 Shells.

## Zwei Ebenen

| Taste | Wirkung |
|-------|---------|
| F1-F12 | Zwischen Desktops (VTs) wechseln |
| Alt+Tab | Zwischen Fenstern innerhalb eines Desktops |

F-Tasten wechseln den Desktop. Alt+Tab cycled durch die Fenster
auf dem aktuellen Desktop. Fokus wechselt, Fenster kommt nach
vorne. Kein Popup-Picker, kein Preview — sofort.

## Terminal ist Default

Jeder Desktop startet mit Terminal. Terminal ist die Shell des
Desktops — wie `init` fuer das System.

```
cosmo$ browser 3              # F3 wird Browser (ersetzt Terminal)
cosmo$ wm 5 terminal browser  # F5: Terminal + Browser nebeneinander
```

Wenn eine App schliesst, kommt das Terminal zurueck.

## Fenster: BeOS Karteikarten

Jedes Fenster hat einen BeOS-Tab — den kleinen Reiter oben am
Fensterrahmen. Kompakt, kein verschwendeter Platz.

```
┌─[Terminal]──────────────────────────┐
│ cosmo$ make                         │
│ ...                                 │
│                                     │
└─────────────────────────────────────┘

┌─[Browser]─┐
│            │
│  example.  │
│  com       │
│            │
└────────────┘
```

Der Tab ist Teil des Fensterrahmens, nicht eine fette Titelleiste.
Greifen und ziehen zum Verschieben.

## Fenster-Operationen

| Operation | Wie |
|-----------|-----|
| Move | Tab greifen und ziehen |
| Resize | Ecke ziehen |
| Close | Tab-X oder Alt+F4 |
| Focus | Klick oder Alt+Tab |

**Kein Minimize.** Fenster sind da oder zu. Kein Verstecken,
kein Dock, kein Taskbar. Alt+Tab oder VT-Wechsel reicht.

Karteikarten auf dem Schreibtisch: stapeln, verschieben, schliessen.
Mehr braucht man nicht.

## Modi pro Desktop

Ein Desktop kann in drei Modi sein:

| Modus | Beschreibung |
|-------|-------------|
| Fullscreen | Ein Fenster, ganzer Bildschirm (Default: Terminal) |
| Frei | BeOS-Karteikarten, frei verschiebbar |
| Tiled | Fenster nebeneinander, automatisch angeordnet |

```
cosmo$ wm 5 terminal browser    # Desktop 5: tiled, zwei Fenster
cosmo$ browser 3                 # Desktop 3: fullscreen Browser
```

## Kein Cruft

- Kein Taskbar
- Kein Dock
- Kein System-Tray
- Kein Minimize
- Kein Maximize-Button (Fullscreen ist ein Modus, kein Button)
- Kein Wallpaper-Picker
- Kein Theme-Engine
- Kein Compositor mit Transparenz und Schatten

Der Desktop ist eine Arbeitsflaeche. Nicht mehr.
