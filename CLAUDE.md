# PristonChess — CLAUDE.md

## Project

Automatisches Schachbrett auf ESP32 (NodeMCU-32S).
Hardware: WS2812B LEDs (8×8), A3144 Hall-Sensoren, 74HC595 Shift-Register, kein e-Paper-Display.
Web-UI läuft im Browser (Handy/PC), ESP32 ist der Server.

## Build & Upload

### Firmware + Web-Assets hochladen

```bash
# Im PlatformIO-Terminal (oder VS Code → PlatformIO: Upload)
pio run --target upload
```

Das löst automatisch aus:
1. `minify.py` — HTML/CSS/JS minifizieren nach `src/web/build/`
2. `prepare_littlefs.py` — Dateien gzippen → `data/`
3. `upload_fs.py` — LittleFS nur hochladen wenn sich `data/` geändert hat (Hash-Check)
4. Firmware flashen

### Nur Filesystem hochladen (ohne Firmware)

```bash
pio run --target uploadfs
```

Oder `.littlefs_hash` löschen, damit `upload_fs.py` einen Upload erzwingt:
```bash
rm .littlefs_hash
pio run --target upload
```

### Seite nicht erreichbar nach Web-Änderungen?

→ `.littlefs_hash` löschen und `pio run --target upload` ausführen.
Das neue board.html (oder andere Web-Dateien) sind erst nach dem LittleFS-Upload auf dem ESP32.

### Serieller Monitor

```bash
pio device monitor --baud 115200
```

## Web-UI

Dateien unter `src/web/`. Änderungen an HTML/CSS/JS werden erst nach einem Upload sichtbar.

- `board.html` — Hauptansicht: Uhren (gespiegelt), Brett, Toolbar
- `index.html` — WiFi-Einstellungen, Spielmodus-Auswahl
- `game.html` — Spielmodus-Auswahl
- `css/styles.css` — Globale Styles (CSS-Variablen, Komponenten)

### board.html Architektur

Vollbild-Layout (100dvh, kein Scroll):
```
#board-shell (Grid: 64px / auto / 1fr / 64px / 52px)
  #clock-black        ← oben, 180° gedreht (für Gegenspieler lesbar)
  #currentGameBanner  ← Spielinfo-Streifen (auto, kollabiert wenn kein Spiel)
  #board-area         ← Brett zentriert (JS berechnet Größe via _resizeBoard())
    #opening-strip    ← Eröffnungsname, overlay oben
    #board            ← chessboardjs
    #eval-container   ← Evaluierungsbalken, overlay unten
  #clock-white        ← unten
  #toolbar-strip      ← Werkzeuge, Nav, Spielaktionen (52px)
```

Fullscreen-Overlays (position: fixed):
- `#status-overlay` — Gespiegelt für SCHACH / Spielende (z-index 200)
- `#noGameBanner` — "Kein Spiel läuft" (z-index 150)
- `#review-panel` — Analysepanel als Bottom-Sheet (z-index 110)
- `#fen-section` — FEN-Editor als Bottom-Sheet (z-index 110)

Alle JavaScript-Element-IDs bleiben erhalten (JS-Kompatibilität).

### Polling

`/board-update` wird alle 500ms abgefragt (JSON mit FEN, Uhren, Status).
`_renderClocks()` läuft alle 100ms client-seitig für flüssige Uhranzeige.

## Firmware-Struktur

```
src/
  main.cpp                    — Haupt-Loop, GameStatusData befüllen
  board_driver.h/.cpp         — WS2812B LEDs + Hall-Sensor-Scan
  wifi_manager_esp32.h/.cpp   — Web-Server, /board-update JSON
  display/display.h/.cpp      — Stub (kein e-Paper mehr)
  chess_bot.cpp               — Stockfish (NNUE)
  chess_engine.h              — isKingInCheck(), Zugvalidierung
```

### GameStatusData (wifi_manager_esp32.h)

Brücke zwischen main.cpp und JSON-API:
```cpp
struct GameStatusData {
  char     turn;          // 'w' oder 'b'
  bool     gameOver;
  char     winnerColor;   // 'w', 'b', 'd'
  char     endReason;     // 'C'=Matt, 'S'=Patt, 'R'=Aufgabe, 'T'=Zeit, ...
  bool     inCheck;
  char     opening[24];
  uint32_t clockWhiteMs, clockBlackMs, clockLimitMs;
  bool     clockTicking, whiteFlagged, blackFlagged;
};
```

In `loop()` befüllen und `wifiManager.setGameStatus(gs)` aufrufen.

## Wichtige Build-Flags

```ini
-DSKIP_CALIBRATION=1   ; Kalibrierung überspringen (keine Hall-Sensoren angeschlossen)
; -DSIMULATION_MODE=1  ; Demo-Partie automatisch starten
```

## Pins (NodeMCU-32S)

| Funktion       | Pin |
|----------------|-----|
| LED (WS2812B)  | 32  |
| SR_CLK         | 14  |
| SR_LATCH       | 26  |
| SR_DATA        | 33  |
| ROW 0–7        | 13, 25, 27, 34, 12, 21, 22, 19 |

## WiFi

- AP: `PristonChess` / `chess123`
- mDNS: `pristonchess.local`
- Gespeicherte Netzwerke: bis zu 3 Profile (NVS)

## Häufige Probleme

| Problem | Lösung |
|---------|--------|
| Seite nicht erreichbar nach HTML-Änderung | `.littlefs_hash` löschen, `pio run --target upload` |
| ESP32 Boot-Loop | `esptool.py --chip esp32 erase_flash` dann neu flashen |
| "Not found" beim AP | Normal — ESP32 ist bereits im Heimnetz, direkt über IP zugreifen |
| IO19 Fehler im Monitor | Hall-Sensor-Pins floaten ohne angeschlossene Sensoren — ignorieren |
| Clang IDE-Fehler (Arduino.h nicht gefunden) | Nur IDE-Fehler, PlatformIO-Build funktioniert normal |
