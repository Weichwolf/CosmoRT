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

## Bar

Immer sichtbar am unteren Bildschirmrand. Zwei fixe Bereiche:

```
┌─────────────────────────────────────────────────┐
│                                                 │
│              Desktop-Inhalt                     │
│                                                 │
├──────────────────────────────┬──────────────────┤
│ [1] 2  3  4  5  6  7  8  9  │  vol net  15:42  │
└──────────────────────────────┴──────────────────┘
  Desktop-Indikator (links)     System-Tray (rechts)
```

**Links:** Desktop 1-12, aktiver hervorgehoben. Klickbar.

**Rechts:** System-Tray. Uhr, Netzwerk, Audio, Batterie.
Fixe Icons, fix positioniert. Nur Kernel/System-Daemons duerfen
Icons setzen. Keine App-Tray-Icons — kein Drittanbieter-Chaos.

Bar und System-Tray werden immer fix positioniert gezeichnet.
Ausnahme: Fullscreen-Modus (siehe unten).

## Maus

System-Cursor wird vom Kernel gezeichnet (Hardware-Cursor).
Immer sichtbar im normalen Desktop-Modus.

Ausnahme: Fullscreen-Modus — kein System-Cursor. App hat
volle Kontrolle und zeichnet eigenen Cursor falls noetig.

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
kein Dock. Alt+Tab oder VT-Wechsel reicht.

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

## Fullscreen

Apps und Games koennen Fullscreen anfordern. Im Fullscreen-Modus:

- **Bar verschwindet** — App hat den ganzen Bildschirm
- **System-Cursor verschwindet** — App zeichnet alles selbst
- **Escape oder Hotkey** bringt Bar und Cursor zurueck

Normaler Desktop: System-Cursor immer da, Bar immer da.
Fullscreen: beides weg, App ist allein.

## Kein Cruft

- Kein Dock
- Kein Minimize
- Kein Maximize-Button (Fullscreen ist ein Modus, kein Button)
- Kein Wallpaper-Picker
- Kein Theme-Engine
- Kein Compositor mit Transparenz und Schatten
- Keine App-Icons im System-Tray

Der Desktop ist eine Arbeitsflaeche. Nicht mehr.
