# Wetter-Dongle — LilyGo T-Dongle-S3

Ein kleiner USB-Stick (LilyGo T-Dongle-S3, ESP32-S3) wird zur eigenständigen
**Wetter-Anzeige**: er holt sich alle 10 Minuten das aktuelle Wetter für
**Baabe/Rügen** von [Open-Meteo](https://open-meteo.com) (kostenlos, kein API-Key)
und zeigt es mit einem gezeichneten Icon, Temperatur, Wetterlage und Wind auf dem
eingebauten 0,96"-Display an. Sobald er per USB Strom hat, läuft das dauerhaft —
kein Rechner nötig.

```
Baabe/Ruegen                    15:15
   ☁  (Icon)            21°C
                        Bewoelkt
Wind 13 km/h
```

## Hardware

- **Board:** LilyGo T-Dongle-S3 (ESP32-S3, 16 MB Flash, natives USB)
- **Display:** ST7735, 0,96", 80×160 Pixel, IPS
- **Verbindung:** nur 2,4-GHz-WLAN (ESP32 kann kein 5 GHz)

### Display-Pinbelegung (LilyGo offiziell)

| Funktion   | GPIO | Hinweis        |
|------------|------|----------------|
| CS         | 4    |                |
| MOSI (SDA) | 3    |                |
| SCLK (SCL) | 5    |                |
| DC         | 2    |                |
| RST (RES)  | 1    |                |
| Backlight  | 38   | **active LOW** |

ST7735-Init: Rotation 1, IPS (Inversion an), 80×160, Offset Spalte 26 / Zeile 1, BGR.

## Inhalt

- **`weather_dongle/`** — die eigentliche Wetter-Firmware.
- **`wifiscan/`** — kleiner Helfer, der alle sichtbaren WLAN-Netze mit Signalstärke
  und Verschlüsselung über die serielle Schnittstelle ausgibt. Praktisch, um den
  exakten SSID-Namen herauszufinden (so wurde z. B. festgestellt, dass das Netz
  „Ferienwohnung MeerZeit" heißt, nicht „Meerzeit").

## Konfiguration

In `weather_dongle/weather_dongle.ino` oben anpassen:

```cpp
const char *WIFI_SSID = "Ferienwohnung MeerZeit";  // exakter WLAN-Name (Groß/Klein!)
const char *WIFI_PASS = "DEIN_WLAN_PASSWORT";                // WLAN-Passwort

const char *ORT_NAME  = "Baabe/Ruegen";
const char *LAT       = "54.3667";   // Sellin: 54.3783
const char *LON       = "13.7000";   // Sellin: 13.6867
```

> ⚠️ **Hinweis:** Die WLAN-Zugangsdaten stehen im Klartext im Quellcode.
> Dieses Repo **nicht öffentlich** veröffentlichen, ohne die Zugangsdaten vorher
> zu entfernen.

## Bauen & Flashen (arduino-cli)

Vorausgesetzt sind `arduino-cli`, der ESP32-Core und die Bibliotheken
`ArduinoJson` (v6) sowie `GFX Library for Arduino`.

```bash
# Einmalig einrichten
arduino-cli config init
arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "ArduinoJson" "GFX Library for Arduino"

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

Die Statuszeilen werden nur einmal beim Start ausgegeben. Daher Gerät zurücksetzen
und sofort mitlesen:

```bash
$ESPTOOL --chip esp32s3 --port /dev/cu.usbmodem2101 \
  --before default_reset --after hard_reset read_mac   # löst Reset aus
stty -f /dev/cu.usbmodem2101 115200 raw -echo
cat /dev/cu.usbmodem2101
```

## Wettercodes

Open-Meteo liefert [WMO-Wettercodes](https://open-meteo.com/en/docs), die im Code
auf Icon-Kategorien gemappt werden: Klar, Heiter, Bewölkt, Nebel, Niesel, Regen,
Schnee, Gewitter.
