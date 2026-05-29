/*
 * Wetter-Dongle fuer LilyGo T-Dongle-S3 (ESP32-S3, 0.96" ST7735 80x160)
 *
 * Holt das aktuelle Wetter von Baabe/Ruegen ueber Open-Meteo (kein API-Key noetig)
 * und zeigt es mit einem einfachen Icon + Text auf dem eingebauten Display an.
 *
 * WLAN: Meerzeit / DEIN_WLAN_PASSWORT
 * Aktualisierung: alle 10 Minuten
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>

// ---------- WLAN ----------
const char *WIFI_SSID = "Ferienwohnung MeerZeit";
const char *WIFI_PASS = "DEIN_WLAN_PASSWORT";

// ---------- Standort (Baabe, Ruegen). Sellin: 54.3783 / 13.6867 ----------
const char *ORT_NAME = "Baabe/Ruegen";
const char *LAT = "54.3667";
const char *LON = "13.7000";

// ---------- Display-Pins (LilyGo T-Dongle-S3, offiziell) ----------
#define TFT_CS    4
#define TFT_SDA   3   // MOSI
#define TFT_SCL   5   // SCLK
#define TFT_DC    2
#define TFT_RES   1
#define TFT_LEDA 38   // Backlight, active LOW

// ST7735 80x160, Offset Spalte 26 / Zeile 1, IPS (Inversion an), BGR
Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCL, TFT_SDA, GFX_NOT_DEFINED);
Arduino_GFX *gfx = new Arduino_ST7735(bus, TFT_RES, 1 /* rotation */, true /* IPS */,
                                      80, 160, 26, 1, 26, 1, true /* BGR */);

const unsigned long UPDATE_MS = 10UL * 60UL * 1000UL;  // 10 Minuten
unsigned long lastUpdate = 0;

// ---------- Wetter-Kategorien ----------
enum WxKind { WX_CLEAR, WX_FEW, WX_CLOUD, WX_FOG, WX_DRIZZLE, WX_RAIN, WX_SNOW, WX_THUNDER };

WxKind codeToKind(int code) {
  switch (code) {
    case 0:                       return WX_CLEAR;
    case 1: case 2:               return WX_FEW;
    case 3:                       return WX_CLOUD;
    case 45: case 48:             return WX_FOG;
    case 51: case 53: case 55:
    case 56: case 57:             return WX_DRIZZLE;
    case 61: case 63: case 65:
    case 66: case 67:
    case 80: case 81: case 82:    return WX_RAIN;
    case 71: case 73: case 75:
    case 77: case 85: case 86:    return WX_SNOW;
    case 95: case 96: case 99:    return WX_THUNDER;
    default:                      return WX_CLOUD;
  }
}

const char *kindText(WxKind k) {
  switch (k) {
    case WX_CLEAR:   return "Klar";
    case WX_FEW:     return "Heiter";
    case WX_CLOUD:   return "Bewoelkt";
    case WX_FOG:     return "Nebel";
    case WX_DRIZZLE: return "Niesel";
    case WX_RAIN:    return "Regen";
    case WX_SNOW:    return "Schnee";
    case WX_THUNDER: return "Gewitter";
  }
  return "?";
}

// ================= Icons (gezeichnet, ca. 60x54 px um cx,cy) =================
void drawSun(int cx, int cy, int r, uint16_t col) {
  gfx->fillCircle(cx, cy, r, col);
  for (int a = 0; a < 360; a += 45) {
    float rad = a * 3.14159265 / 180.0;
    int x1 = cx + cos(rad) * (r + 3);
    int y1 = cy + sin(rad) * (r + 3);
    int x2 = cx + cos(rad) * (r + 9);
    int y2 = cy + sin(rad) * (r + 9);
    gfx->drawLine(x1, y1, x2, y2, col);
  }
}

void drawCloud(int cx, int cy, uint16_t col) {
  gfx->fillCircle(cx - 11, cy, 8, col);
  gfx->fillCircle(cx + 11, cy, 8, col);
  gfx->fillCircle(cx, cy - 7, 11, col);
  gfx->fillRect(cx - 11, cy, 23, 9, col);
}

void drawRainDrops(int cx, int cy, uint16_t col) {
  for (int i = -1; i <= 1; i++)
    gfx->drawLine(cx + i * 9, cy, cx + i * 9 - 3, cy + 9, col);
}

void drawSnowFlakes(int cx, int cy, uint16_t col) {
  for (int i = -1; i <= 1; i++) gfx->fillCircle(cx + i * 9, cy + 5, 2, col);
}

void drawBolt(int cx, int cy, uint16_t col) {
  gfx->fillTriangle(cx, cy, cx - 6, cy + 10, cx + 1, cy + 9, col);
  gfx->fillTriangle(cx + 1, cy + 6, cx + 7, cy + 5, cx, cy + 16, col);
}

void drawIcon(WxKind k, int cx, int cy) {
  switch (k) {
    case WX_CLEAR:
      drawSun(cx, cy, 16, YELLOW);
      break;
    case WX_FEW:
      drawSun(cx - 8, cy - 8, 11, YELLOW);
      drawCloud(cx + 4, cy + 6, LIGHTGREY);
      break;
    case WX_CLOUD:
      drawCloud(cx, cy, DARKGREY);
      break;
    case WX_FOG:
      drawCloud(cx, cy - 4, LIGHTGREY);
      for (int i = 0; i < 3; i++)
        gfx->drawFastHLine(cx - 16, cy + 12 + i * 5, 32, LIGHTGREY);
      break;
    case WX_DRIZZLE:
      drawCloud(cx, cy - 6, LIGHTGREY);
      drawRainDrops(cx, cy + 12, CYAN);
      break;
    case WX_RAIN:
      drawCloud(cx, cy - 6, DARKGREY);
      drawRainDrops(cx, cy + 12, CYAN);
      drawRainDrops(cx, cy + 18, CYAN);
      break;
    case WX_SNOW:
      drawCloud(cx, cy - 6, LIGHTGREY);
      drawSnowFlakes(cx, cy + 12, WHITE);
      break;
    case WX_THUNDER:
      drawCloud(cx, cy - 6, DARKGREY);
      drawBolt(cx, cy + 8, YELLOW);
      break;
  }
}

// ================= Bildschirm-Ausgaben =================
void showStatus(const char *line1, const char *line2, uint16_t col) {
  gfx->fillScreen(BLACK);
  gfx->setTextColor(col);
  gfx->setTextSize(2);
  gfx->setCursor(6, 18);
  gfx->print(line1);
  if (line2 && line2[0]) {
    gfx->setTextSize(1);
    gfx->setCursor(6, 48);
    gfx->print(line2);
  }
}

void drawWeather(float temp, WxKind kind, float wind, const char *uhrzeit) {
  gfx->fillScreen(BLACK);

  // Kopfzeile: Ort links, Uhrzeit rechts
  gfx->setTextSize(1);
  gfx->setTextColor(CYAN);
  gfx->setCursor(4, 4);
  gfx->print(ORT_NAME);
  if (uhrzeit && uhrzeit[0]) {
    gfx->setTextColor(DARKGREY);
    gfx->setCursor(160 - 6 * (int)strlen(uhrzeit) - 4, 4);
    gfx->print(uhrzeit);
  }

  // Icon links
  drawIcon(kind, 36, 44);

  // Temperatur gross rechts
  int t = (int)lroundf(temp);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", t);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(4);
  int tx = 80;
  gfx->setCursor(tx, 22);
  gfx->print(buf);
  // Grad-Zeichen + C
  int gx = tx + (int)strlen(buf) * 24 + 3;
  gfx->drawCircle(gx + 3, 26, 3, WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(gx, 38);
  gfx->print("C");

  // Wetterlage rechts unten
  gfx->setTextColor(YELLOW);
  gfx->setTextSize(1);
  gfx->setCursor(80, 58);
  gfx->print(kindText(kind));

  // Wind unten links
  gfx->setTextColor(LIGHTGREY);
  gfx->setCursor(6, 70);
  gfx->printf("Wind %d km/h", (int)lroundf(wind));
}

// ================= WLAN =================
bool connectWifi() {
  showStatus("WLAN...", WIFI_SSID, CYAN);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
    Serial.print(".");
  }
  bool ok = WiFi.status() == WL_CONNECTED;
  Serial.println(ok ? ("\nWLAN verbunden, IP " + WiFi.localIP().toString()) : "\nWLAN FEHLGESCHLAGEN");
  return ok;
}

// ================= Wetter holen =================
bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!connectWifi()) {
      showStatus("Kein WLAN", "neuer Versuch...", RED);
      return false;
    }
  }

  String url = String("https://api.open-meteo.com/v1/forecast?latitude=") + LAT +
               "&longitude=" + LON +
               "&current=temperature_2m,weather_code,is_day,wind_speed_10m" +
               "&timezone=Europe%2FBerlin";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.setTimeout(15000);
  if (!https.begin(client, url)) {
    showStatus("Fehler", "begin()", RED);
    return false;
  }
  int code = https.GET();
  if (code != 200) {
    https.end();
    char msg[24];
    snprintf(msg, sizeof(msg), "HTTP %d", code);
    showStatus("Server-Fehler", msg, RED);
    return false;
  }
  String payload = https.getString();
  https.end();

  StaticJsonDocument<256> filter;
  filter["current"]["temperature_2m"] = true;
  filter["current"]["weather_code"]   = true;
  filter["current"]["wind_speed_10m"] = true;
  filter["current"]["time"]           = true;

  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) {
    showStatus("JSON-Fehler", err.c_str(), RED);
    return false;
  }

  float temp = doc["current"]["temperature_2m"] | 0.0f;
  int   wc   = doc["current"]["weather_code"]   | -1;
  float wind = doc["current"]["wind_speed_10m"] | 0.0f;
  const char *tIso = doc["current"]["time"] | "";

  Serial.printf("Wetter %s: %.1f C, code %d (%s), Wind %.0f km/h, Zeit %s\n",
                ORT_NAME, temp, wc, kindText(codeToKind(wc)), wind, tIso);

  // Uhrzeit HH:MM aus "2026-05-29T15:20"
  char uhr[6] = "";
  if (strlen(tIso) >= 16) { strncpy(uhr, tIso + 11, 5); uhr[5] = 0; }

  drawWeather(temp, codeToKind(wc), wind, uhr);
  return true;
}

void setup() {
  Serial.begin(115200);

  pinMode(TFT_LEDA, OUTPUT);
  digitalWrite(TFT_LEDA, LOW);  // Backlight an (active LOW)

  gfx->begin();
  gfx->fillScreen(BLACK);

  showStatus("Wetter", "Dongle startet", WHITE);
  delay(800);

  if (fetchWeather()) lastUpdate = millis();
  else                lastUpdate = millis() - UPDATE_MS + 15000;  // bald erneut
}

void loop() {
  if (millis() - lastUpdate >= UPDATE_MS) {
    if (fetchWeather()) lastUpdate = millis();
    else                lastUpdate = millis() - UPDATE_MS + 15000;  // in 15s erneut
  }
  delay(500);
}
