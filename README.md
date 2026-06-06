# pristonchess

Automatisches Schachbrett auf ESP32 (NodeMCU-32S) mit WS2812B-LEDs, A3144 Hall-Sensoren
und Web-UI. Der ESP32 ist Server; das Brett wird im Browser (Handy/PC) bedient.

## Build & Upload

```bash
pio run --target upload       # Firmware + Web-Assets per USB flashen
pio run --target uploadfs     # nur LittleFS (Web-Dateien)
pio device monitor --baud 115200
```

Details und Hardware-/Pin-Belegung siehe [CLAUDE.md](CLAUDE.md).

## OTA / Entferntes Flashen

Das Brett kann ohne USB-Kabel aktualisiert werden. Update-Quelle ist das
*latest release* dieses Repos (`OTA_GITHUB_API_URL` in `src/version.h`).

### Manuell per Browser (gleiches Netz)

1. `pio run` → `.pio/build/nodemcu-32s/firmware.bin`
2. Einstellungsseite öffnen (`pristonchess.local` bzw. IP) → OTA-Bereich
3. `.bin` (Firmware) oder `.tar` (Web-Assets) ins Upload-Feld ziehen.
   Firmware flasht und rebootet automatisch.

### Über Internet per GitHub-Release

Beim Boot prüft das Brett das neueste Release und vergleicht den Tag mit der
laufenden `FIRMWARE_VERSION`. Bei neuerer Version werden `firmware.bin` und
`web_assets.tar` aus dem Release geladen und angewendet.

**Neues Release erstellen (automatisch):**

```bash
git tag v1.0.0
git push origin v1.0.0
```

Der GitHub-Actions-Workflow [`.github/workflows/release.yml`](.github/workflows/release.yml)
baut `firmware.bin` + `web_assets.tar`, schreibt den Tag als `FIRMWARE_VERSION` in die
Firmware und legt das Release mit beiden Assets an. Anschließend im Web-UI
„Auf Updates prüfen“ → „Anwenden“ (oder Auto-Update aktivieren).

**Neues Release erstellen (manuell, ohne CI):**

```bash
pio run
cp .pio/build/nodemcu-32s/firmware.bin firmware.bin
( cd data && tar -cf ../web_assets.tar * )
```

Dann ein GitHub-Release mit Tag `vX.Y.Z` anlegen und beide Dateien als Assets
anhängen. Die Tag-Version muss höher sein als die laufende `FIRMWARE_VERSION`.
