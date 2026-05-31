# Wetter-Dongle DELUXE — LilyGo T-Dongle-S3

Ein kleiner USB-Stick (LilyGo T-Dongle-S3, ESP32-S3) wird zur animierten
**Wetterstation**: er holt sich alle 10 Minuten alles, was die kostenlose
[Open-Meteo](https://open-meteo.com)-API hergibt (kein API-Key), und zeigt es als
**rotierendes Karten-Carousel** mit flüssigen Übergängen, animierten Icons und
einer als Wetterstimmung leuchtenden RGB-LED. Sobald der Stick per USB Strom hat,
läuft alles eigenständig — kein Rechner nötig.

Standort: **Baabe/Rügen** (Sellin-Koordinaten als Kommentar im Code).

## Die Karten (wechseln alle 5 Sekunden)

| Karte       | Inhalt                                                                 |
|-------------|------------------------------------------------------------------------|
| **JETZT**   | Temperatur groß, animiertes Wetter-Icon, Wetterlage, gefühlte Temp.    |
| **WIND**    | Geschwindigkeit, Böen, Beaufort, Richtung + animierter Kompass         |
| **LUFT**    | Luftfeuchte (animierter Wassertropfen), Wolken %, Luftdruck, UV-Index  |
| **HEUTE**   | Min/Max-Thermometer, Niederschlagsmenge + Regenwahrscheinlichkeit      |
| **SONNE**   | Sonnenauf-/untergang, Tageslänge, Sonne wandert über einen Bogen       |
| **3 TAGE**  | Mini-Vorhersage mit Icons und Max/Min für die nächsten drei Tage       |

### Animationen & Extras

- **Flüssige Slide-Übergänge** zwischen den Karten dank Off-Screen-Framebuffer
  (`Arduino_Canvas`) — flackerfrei bei ~30 FPS.
- **Animierte Icons:** rotierende/pulsierende Sonne, driftende Wolken, fallender
  Regen/Schnee, blitzendes Gewitter, ziehender Nebel.
- **Weicher Glow-Halo** hinter dem großen Wetter-Icon (und der wandernden Sonne):
  pulsierender Schein in der jeweiligen Wetter-Stimmungsfarbe — dieselbe Farbe wie
  die RGB-LED.
- **Animierter Kompass** mit sanft einschwenkender Nadel, **wandernde Sonne** auf
  der Sonnen-Karte, **atmende Balken**.
- **Eingebaute APA102-RGB-LED** als Ambientelicht: Farbe spiegelt das Wetter
  (Sonne = warmgelb, Regen = blau, Schnee = eisblau, Gewitter = violett mit
  Blitz-Flash, Nacht = dunkelblau), mit sanftem „Atmen".
- **Ruckelfreies Update:** der Wetterabruf läuft als eigener Task auf Core 0,
  während Animation und LED auf Core 1 ungestört bei ~30 FPS weiterlaufen — kein
  Stocken beim 10-Minuten-Abruf. Das JSON wird direkt aus dem HTTP-Stream geparst
  (HTTP/1.0, gefiltert), das spart RAM.

## Genutzte API-Felder

`current`: `temperature_2m`, `relative_humidity_2m`, `apparent_temperature`,
`is_day`, `precipitation`, `weather_code`, `cloud_cover`, `pressure_msl`,
`wind_speed_10m`, `wind_direction_10m`, `wind_gusts_10m`

`daily` (4 Tage): `weather_code`, `temperature_2m_max`, `temperature_2m_min`,
`sunrise`, `sunset`, `uv_index_max`, `precipitation_sum`,
`precipitation_probability_max`

## Hardware

- **Board:** LilyGo T-Dongle-S3 (ESP32-S3, 16 MB Flash, natives USB)
- **Display:** ST7735, 0,96", 80×160 Pixel, IPS
- **RGB-LED:** APA102 (DotStar), onboard
- **WLAN:** nur 2,4 GHz (ESP32 kann kein 5 GHz)

### Pinbelegung (LilyGo offiziell)

| Funktion   | GPIO | Hinweis        |
|------------|------|----------------|
| TFT CS     | 4    |                |
| TFT MOSI   | 3    |                |
| TFT SCLK   | 5    |                |
| TFT DC     | 2    |                |
| TFT RST    | 1    |                |
| Backlight  | 38   | **active LOW** |
| LED Data   | 40   | APA102         |
| LED Clock  | 39   | APA102         |

ST7735-Init: Rotation 1, IPS (Inversion an), 80×160, Offset Spalte 26 / Zeile 1, BGR.

## Inhalt

- **`weather_dongle/`** — die animierte Wetter-Firmware (Karten-Carousel + LED).
- **`wifiscan/`** — Helfer, der alle sichtbaren WLAN-Netze mit Signalstärke und
  Verschlüsselung über die serielle Schnittstelle ausgibt. Damit wurde der exakte
  SSID-Name gefunden (das Netz heißt „Ferienwohnung MeerZeit", nicht „Meerzeit").

## Konfiguration

**WLAN-Zugangsdaten** liegen in `weather_dongle/secrets.h` — diese Datei ist per
`.gitignore` ausgeschlossen und landet nie im Repo. Zum Einrichten die Vorlage kopieren
und ausfüllen:

```sh
cp weather_dongle/secrets.h.example weather_dongle/secrets.h
```

```cpp
// secrets.h
#define WIFI_SSID "Ferienwohnung MeerZeit"  // exakter Name (Groß/Klein!)
#define WIFI_PASS "MeinPasswort"
```

**Standort** oben in `weather_dongle/weather_dongle.ino`:

```cpp
const char *ORT_NAME  = "Baabe";
const char *LAT       = "54.3667";   // Sellin: 54.3783
const char *LON       = "13.7000";   // Sellin: 13.6867
```

## Bauen & Flashen (arduino-cli)

Benötigt `arduino-cli`, den ESP32-Core und die Bibliotheken `ArduinoJson` (v6),
`GFX Library for Arduino` und `FastLED`.

```bash
# Einmalig einrichten
arduino-cli config init
arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "ArduinoJson" "GFX Library for Arduino" "FastLED"

# Kompilieren
arduino-cli compile -b esp32:esp32:esp32s3:CDCOnBoot=cdc,UploadSpeed=115200 weather_dongle
```

### Flashen — wichtiger Stolperstein

Der normale `arduino-cli upload` schlägt über den nativen USB-JTAG des ESP32-S3
gerne fehl (Wechsel auf 921600 Baud → *„No serial data received"*). Zuverlässig
funktioniert das gebündelte `esptool` **ohne Stub** bei 115200 Baud:

```bash
ESPTOOL=~/Library/Arduino15/packages/esp32/tools/esptool_py/4.2.1/esptool
SK=~/Library/Caches/arduino/sketches/<HASH>          # Pfad aus 'compile'-Ausgabe
BOOT_APP0=~/Library/Arduino15/packages/esp32/hardware/esp32/2.0.5/tools/partitions/boot_app0.bin

"$ESPTOOL" --chip esp32s3 --port /dev/cu.usbmodem2101 --baud 115200 --no-stub \
  --before default_reset --after hard_reset write_flash -z \
  --flash_mode dio --flash_freq 80m --flash_size keep \
  0x0     "$SK/weather_dongle.ino.bootloader.bin" \
  0x8000  "$SK/weather_dongle.ino.partitions.bin" \
  0xe000  "$BOOT_APP0" \
  0x10000 "$SK/weather_dongle.ino.bin"
```

Den genauen Sketch-Pfad und die FQBN liefert:
`arduino-cli upload ... --verbose --dry-run`.

### Serielle Ausgabe mitlesen (Debug)

Die Statuszeilen kommen nur einmal beim Start. Daher Gerät zurücksetzen und sofort
mitlesen:

```bash
$ESPTOOL --chip esp32s3 --port /dev/cu.usbmodem2101 \
  --before default_reset --after hard_reset read_mac   # löst Reset aus
stty -f /dev/cu.usbmodem2101 115200 raw -echo
cat /dev/cu.usbmodem2101
```

## Technik-Notizen

- **Framebuffer:** Es wird komplett in `Arduino_Canvas` (RAM, 160×80×2 ≈ 25 KB)
  gezeichnet und dann per `flush()` ans Display geschoben — daher flackerfrei.
- **Verbindung:** Open-Meteo wird über **HTTP** abgerufen (spart TLS-RAM, kein
  Zertifikat nötig).
- **Wettercodes:** Open-Meteo liefert WMO-Codes, die im Code auf Icon-Kategorien
  gemappt werden (Klar, Heiter, Bewölkt, Nebel, Niesel, Regen, Schnee, Gewitter).
