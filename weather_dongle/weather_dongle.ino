/*
 * Wetter-Dongle DELUXE  -  LilyGo T-Dongle-S3 (ESP32-S3, 0.96" ST7735 80x160)
 *
 * Karten-Carousel mit allem, was die kostenlose Open-Meteo-API hergibt:
 *   JETZT  -  Temperatur, gefuehlt, animiertes Wetter-Icon
 *   WIND   -  Geschwindigkeit, Boeen, Beaufort, animierter Kompass
 *   LUFT   -  Feuchte (Tropfen-Fuellung), Wolken, Luftdruck, UV
 *   HEUTE  -  Min/Max-Thermometer, Niederschlag + Wahrscheinlichkeit
 *   SONNE  -  Sonnenauf-/untergang, Tageslaenge, Sonne wandert ueber Bogen
 *   3 TAGE -  Mini-Vorhersage mit Icons
 *
 * Fluessige Slide-Uebergaenge per Framebuffer (Arduino_Canvas).
 * Eingebaute APA102-RGB-LED zeigt die Wetterstimmung als Ambientelicht.
 *
 * Standort: Baabe/Ruegen. Aktualisierung alle 10 Minuten.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <FastLED.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <math.h>

// ---------- WLAN ----------
const char *WIFI_SSID = "Ferienwohnung MeerZeit";
const char *WIFI_PASS = "DEIN_WLAN_PASSWORT";

// ---------- Standort (Baabe). Sellin: 54.3783 / 13.6867 ----------
const char *ORT_NAME = "Baabe";
const char *LAT = "54.3667";
const char *LON = "13.7000";

// ---------- Display-Pins (LilyGo T-Dongle-S3) ----------
#define TFT_CS    4
#define TFT_SDA   3
#define TFT_SCL   5
#define TFT_DC    2
#define TFT_RES   1
#define TFT_LEDA 38   // Backlight active LOW
// APA102 RGB-LED
#define LED_DI   40
#define LED_CI   39

#define SCR_W 160
#define SCR_H 80

Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCL, TFT_SDA, GFX_NOT_DEFINED);
Arduino_GFX *panel = new Arduino_ST7735(bus, TFT_RES, 1 /*rot*/, true /*IPS*/, 80, 160, 26, 1, 26, 1, true /*BGR*/);
Arduino_GFX *gfx = new Arduino_Canvas(SCR_W, SCR_H, panel);  // Framebuffer fuer flackerfreie Animation

CRGB led[1];

// ---------- Datenmodell ----------
enum WxKind { WX_CLEAR, WX_FEW, WX_CLOUD, WX_FOG, WX_DRIZZLE, WX_RAIN, WX_SNOW, WX_THUNDER };

struct Wx {
  bool  valid = false;
  float temp = 0, feels = 0, hum = 0, press = 0, cloud = 0, precip = 0;
  float wind = 0, gust = 0;
  int   windDir = 0, wcode = -1, isDay = 1;
  float tmax = 0, tmin = 0, precipSum = 0, uv = 0;
  int   precipProb = 0;
  char  sunrise[6] = "--:--", sunset[6] = "--:--", updated[6] = "--:--";
  int   fcode[3] = {-1, -1, -1};
  float fmax[3] = {0}, fmin[3] = {0};
  char  fday[3][4] = {"", "", ""};
} wx;

// ---------- Hintergrund-Abruf (laeuft auf Core 0, damit die Animation nie ruckelt) ----------
Wx wxNew;                       // Staging-Puffer: nur der Wetter-Task schreibt hier hinein
SemaphoreHandle_t wxMutex;      // schuetzt wxNew / wxReady gegen gleichzeitigen Zugriff
volatile bool wxReady = false;  // true = frische Daten warten auf Uebernahme durch loop()
volatile bool online  = false;  // war der letzte Abruf erfolgreich?

WxKind codeToKind(int c) {
  switch (c) {
    case 0: return WX_CLEAR;
    case 1: case 2: return WX_FEW;
    case 3: return WX_CLOUD;
    case 45: case 48: return WX_FOG;
    case 51: case 53: case 55: case 56: case 57: return WX_DRIZZLE;
    case 61: case 63: case 65: case 66: case 67: case 80: case 81: case 82: return WX_RAIN;
    case 71: case 73: case 75: case 77: case 85: case 86: return WX_SNOW;
    case 95: case 96: case 99: return WX_THUNDER;
    default: return WX_CLOUD;
  }
}
const char *kindText(WxKind k) {
  switch (k) {
    case WX_CLEAR: return "Klar"; case WX_FEW: return "Heiter"; case WX_CLOUD: return "Bewoelkt";
    case WX_FOG: return "Nebel"; case WX_DRIZZLE: return "Niesel"; case WX_RAIN: return "Regen";
    case WX_SNOW: return "Schnee"; case WX_THUNDER: return "Gewitter";
  }
  return "?";
}
const char *dirText(int deg) {
  static const char *d[8] = {"N", "NO", "O", "SO", "S", "SW", "W", "NW"};
  return d[((deg + 22) / 45) % 8];
}
int beaufort(float kmh) {
  const float t[] = {1, 6, 12, 20, 29, 39, 50, 62, 75, 89, 103, 118};
  int b = 0; for (int i = 0; i < 12; i++) if (kmh >= t[i]) b = i + 1; return b;
}
uint16_t uvColor(float uv) {
  if (uv < 3) return GREEN; if (uv < 6) return YELLOW; if (uv < 8) return ORANGE;
  if (uv < 11) return RED; return MAGENTA;
}
const char *wdayName(const char *iso) {  // iso "YYYY-MM-DD"
  static const char *n[7] = {"Sa", "So", "Mo", "Di", "Mi", "Do", "Fr"};
  if (strlen(iso) < 10) return "?";
  int y = atoi(iso), m = atoi(iso + 5), d = atoi(iso + 8);
  if (m < 3) { m += 12; y--; }
  int K = y % 100, J = y / 100;
  int h = (d + 13 * (m + 1) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
  return n[h];
}

// ================= kleine Zeichen-Helfer =================
void tprint(int x, int y, uint8_t size, uint16_t col, const char *s) {
  gfx->setTextSize(size); gfx->setTextColor(col); gfx->setCursor(x, y); gfx->print(s);
}
void degMark(int x, int y, uint16_t col) { gfx->drawCircle(x, y, 2, col); }

// ================= animierte Icons =================
void iconSun(int cx, int cy, int r, float ph, uint16_t col) {
  float pr = r + sinf(ph * 2.0f) * 1.5f;              // pulsiert
  for (int a = 0; a < 360; a += 30) {
    float rad = (a + ph * 35.0f) * DEG_TO_RAD;          // rotiert
    int x1 = cx + cosf(rad) * (pr + 3), y1 = cy + sinf(rad) * (pr + 3);
    int x2 = cx + cosf(rad) * (pr + 9), y2 = cy + sinf(rad) * (pr + 9);
    gfx->drawLine(x1, y1, x2, y2, col);
  }
  gfx->fillCircle(cx, cy, pr, col);
}
void cloudShape(int cx, int cy, uint16_t col) {
  gfx->fillCircle(cx - 11, cy, 8, col); gfx->fillCircle(cx + 11, cy, 8, col);
  gfx->fillCircle(cx, cy - 7, 11, col); gfx->fillRect(cx - 11, cy, 23, 9, col);
}
void iconCloud(int cx, int cy, float ph, uint16_t col) {
  cloudShape(cx + (int)(sinf(ph) * 3), cy, col);        // driftet
}
void iconRain(int cx, int cy, float ph, uint16_t cloudCol, int drops) {
  cloudShape(cx, cy - 6, cloudCol);
  for (int i = 0; i < drops; i++) {
    float p = fmodf(ph * 2.2f + i * 0.37f, 1.0f);
    int dx = cx - 12 + (i % 3) * 12;
    int dy = cy + 4 + (int)(p * 18);
    gfx->drawLine(dx, dy, dx - 2, dy + 5, CYAN);
  }
}
void iconSnow(int cx, int cy, float ph, uint16_t cloudCol) {
  cloudShape(cx, cy - 6, cloudCol);
  for (int i = 0; i < 4; i++) {
    float p = fmodf(ph * 0.9f + i * 0.3f, 1.0f);
    int dx = cx - 13 + (i * 9) + (int)(sinf(ph * 2 + i) * 2);
    int dy = cy + 4 + (int)(p * 18);
    gfx->fillCircle(dx, dy, 1, WHITE);
  }
}
void iconThunder(int cx, int cy, float ph, uint16_t cloudCol) {
  cloudShape(cx, cy - 6, cloudCol);
  bool flash = fmodf(ph, 2.2f) < 0.18f;                 // blitzt gelegentlich
  uint16_t c = flash ? WHITE : YELLOW;
  gfx->fillTriangle(cx, cy + 2, cx - 6, cy + 12, cx + 1, cy + 11, c);
  gfx->fillTriangle(cx + 1, cy + 8, cx + 7, cy + 7, cx, cy + 18, c);
}
void iconFog(int cx, int cy, float ph, uint16_t col) {
  cloudShape(cx, cy - 6, col);
  for (int i = 0; i < 3; i++) {
    int off = (int)(sinf(ph * 1.5f + i) * 4);
    gfx->drawFastHLine(cx - 16 + off, cy + 10 + i * 5, 30, LIGHTGREY);
  }
}
void drawIconBig(WxKind k, int cx, int cy, float ph) {
  switch (k) {
    case WX_CLEAR: iconSun(cx, cy, 14, ph, wx.isDay ? YELLOW : LIGHTGREY); break;
    case WX_FEW: iconSun(cx - 7, cy - 7, 9, ph, YELLOW); iconCloud(cx + 5, cy + 6, ph, LIGHTGREY); break;
    case WX_CLOUD: iconCloud(cx, cy, ph, DARKGREY); break;
    case WX_FOG: iconFog(cx, cy, ph, LIGHTGREY); break;
    case WX_DRIZZLE: iconRain(cx, cy, ph, LIGHTGREY, 3); break;
    case WX_RAIN: iconRain(cx, cy, ph, DARKGREY, 6); break;
    case WX_SNOW: iconSnow(cx, cy, ph, LIGHTGREY); break;
    case WX_THUNDER: iconThunder(cx, cy, ph, DARKGREY); break;
  }
}
void drawIconSmall(WxKind k, int cx, int cy) {  // statisch, klein (fuer Vorhersage)
  switch (k) {
    case WX_CLEAR: for (int a = 0; a < 360; a += 45) { float r = a * DEG_TO_RAD; gfx->drawLine(cx + cosf(r) * 7, cy + sinf(r) * 7, cx + cosf(r) * 10, cy + sinf(r) * 10, YELLOW);} gfx->fillCircle(cx, cy, 5, YELLOW); break;
    case WX_FEW: gfx->fillCircle(cx - 3, cy - 3, 4, YELLOW); gfx->fillCircle(cx + 2, cy + 2, 5, LIGHTGREY); break;
    case WX_CLOUD: case WX_FOG: gfx->fillCircle(cx - 5, cy, 4, DARKGREY); gfx->fillCircle(cx + 5, cy, 4, DARKGREY); gfx->fillCircle(cx, cy - 3, 5, DARKGREY); gfx->fillRect(cx - 5, cy, 11, 4, DARKGREY); break;
    case WX_DRIZZLE: case WX_RAIN: gfx->fillCircle(cx, cy - 3, 5, DARKGREY); gfx->fillRect(cx - 5, cy - 3, 11, 4, DARKGREY); for (int i = -1; i <= 1; i++) gfx->drawLine(cx + i * 4, cy + 3, cx + i * 4 - 1, cy + 7, CYAN); break;
    case WX_SNOW: gfx->fillCircle(cx, cy - 3, 5, LIGHTGREY); gfx->fillRect(cx - 5, cy - 3, 11, 4, LIGHTGREY); for (int i = -1; i <= 1; i++) gfx->fillCircle(cx + i * 4, cy + 5, 1, WHITE); break;
    case WX_THUNDER: gfx->fillCircle(cx, cy - 3, 5, DARKGREY); gfx->fillRect(cx - 5, cy - 3, 11, 4, DARKGREY); gfx->fillTriangle(cx, cy, cx - 3, cy + 6, cx + 1, cy + 5, YELLOW); break;
  }
}

// ================= Karten =================
void header(int x, const char *title) {
  tprint(x + 4, 3, 1, CYAN, title);
  tprint(x + SCR_W - 4 - 6 * (int)strlen(wx.updated), 3, 1, DARKGREY, wx.updated);
  gfx->drawFastHLine(x + 4, 13, SCR_W - 8, gfx->color565(40, 40, 50));
}

void cardNow(int x, float ph) {
  header(x, ORT_NAME);
  drawIconBig(codeToKind(wx.wcode), x + 40, 44, ph);
  char b[8]; snprintf(b, sizeof(b), "%d", (int)lroundf(wx.temp));
  tprint(x + 78, 22, 4, WHITE, b);
  int gx = x + 78 + (int)strlen(b) * 24 + 3;
  degMark(gx + 3, 26, WHITE); tprint(gx, 38, 2, WHITE, "C");
  tprint(x + 78, 52, 1, YELLOW, kindText(codeToKind(wx.wcode)));
  char f[16]; snprintf(f, sizeof(f), "gef. %d C", (int)lroundf(wx.feels));
  tprint(x + 78, 65, 1, LIGHTGREY, f);
}

void cardWind(int x, float ph) {
  static float needle = 0;
  float target = wx.windDir * DEG_TO_RAD;
  // sanft einschwenken (kuerzester Weg)
  float diff = atan2f(sinf(target - needle), cosf(target - needle));
  needle += diff * 0.12f;
  header(x, "WIND");
  int cx = x + 36, cy = 44, r = 26;
  gfx->drawCircle(cx, cy, r, gfx->color565(60, 60, 80));
  tprint(cx - 2, cy - r - 1, 1, DARKGREY, "N");
  tprint(cx - 2, cy + r - 6, 1, DARKGREY, "S");
  // Pfeil (zeigt Richtung, aus der der Wind kommt -> Spitze dorthin)
  float a = needle - PI / 2;
  int tipx = cx + cosf(a) * (r - 3), tipy = cy + sinf(a) * (r - 3);
  int bx = cx - cosf(a) * (r - 8), by = cy - sinf(a) * (r - 8);
  int lx = cx + cosf(a + 2.5f) * 7, ly = cy + sinf(a + 2.5f) * 7;
  int rx = cx + cosf(a - 2.5f) * 7, ry = cy + sinf(a - 2.5f) * 7;
  gfx->drawLine(bx, by, tipx, tipy, ORANGE);
  gfx->fillTriangle(tipx, tipy, lx, ly, rx, ry, ORANGE);
  gfx->fillCircle(cx, cy, 2, WHITE);
  // rechts: Werte
  char b[10]; snprintf(b, sizeof(b), "%d", (int)lroundf(wx.wind));
  tprint(x + 80, 18, 3, WHITE, b);
  tprint(x + 80 + (int)strlen(b) * 18 + 2, 30, 1, LIGHTGREY, "km/h");
  char g[16]; snprintf(g, sizeof(g), "Boe %d", (int)lroundf(wx.gust));
  tprint(x + 80, 44, 1, CYAN, g);
  char bf[16]; snprintf(bf, sizeof(bf), "%d Bft  %s", beaufort(wx.wind), dirText(wx.windDir));
  tprint(x + 80, 58, 1, YELLOW, bf);
}

void barH(int x, int y, int w, int h, float frac, uint16_t col) {
  if (frac < 0) frac = 0; if (frac > 1) frac = 1;
  gfx->drawRect(x, y, w, h, gfx->color565(60, 60, 70));
  gfx->fillRect(x + 1, y + 1, (int)((w - 2) * frac), h - 2, col);
}

void cardAtmo(int x, float ph) {
  header(x, "LUFT");
  // animierter Tropfen mit Fuellstand = Feuchte
  int cx = x + 28, cy = 48;
  float fill = wx.hum / 100.0f;
  gfx->fillTriangle(cx, cy - 22, cx - 11, cy + 4, cx + 11, cy + 4, gfx->color565(20, 30, 60));
  gfx->fillCircle(cx, cy + 6, 13, gfx->color565(20, 30, 60));
  int top = cy + 19 - (int)(fill * 38);
  for (int yy = top; yy <= cy + 18; yy++) {
    int half = (yy < cy + 4) ? (int)((yy - (cy - 22)) * 11.0f / 26.0f) : (int)(sqrtf(fmaxf(0, 169 - (yy - (cy + 6)) * (yy - (cy + 6)))));
    int wob = (int)(sinf(ph * 3 + yy * 0.3f) * 1.0f);
    gfx->drawFastHLine(cx - half + wob, yy, 2 * half, gfx->color565(0, 120, 230));
  }
  char h[12]; snprintf(h, sizeof(h), "%d%%", (int)lroundf(wx.hum));
  tprint(cx - (int)strlen(h) * 3, cy + 26, 1, WHITE, h);
  // rechts: Werte-Liste
  int rx = x + 64;
  char c[18];
  snprintf(c, sizeof(c), "Wolken %d%%", (int)lroundf(wx.cloud)); tprint(rx, 20, 1, LIGHTGREY, c);
  snprintf(c, sizeof(c), "Druck %d", (int)lroundf(wx.press));    tprint(rx, 34, 1, LIGHTGREY, c);
  tprint(rx, 48, 1, LIGHTGREY, "hPa");
  snprintf(c, sizeof(c), "UV %.0f", wx.uv);                      tprint(rx, 62, 1, uvColor(wx.uv), c);
}

void cardToday(int x, float ph) {
  header(x, "HEUTE");
  // Thermometer min..max
  int tx = x + 18, ty0 = 22, ty1 = 60;
  gfx->drawRoundRect(tx - 4, ty0 - 2, 9, ty1 - ty0 + 6, 4, gfx->color565(80, 80, 90));
  gfx->fillCircle(tx, ty1 + 8, 7, RED);
  gfx->fillRect(tx - 2, ty0, 5, ty1 - ty0 + 8, RED);
  char mx[8], mn[8];
  snprintf(mx, sizeof(mx), "%d", (int)lroundf(wx.tmax));
  snprintf(mn, sizeof(mn), "%d", (int)lroundf(wx.tmin));
  tprint(tx + 12, ty0 - 2, 2, ORANGE, mx); tprint(tx + 12, 2 + ty0 + 16, 1, ORANGE, "max");
  tprint(tx + 12, ty1 - 6, 1, CYAN, "min "); tprint(tx + 38, ty1 - 9, 2, CYAN, mn);
  // Niederschlag
  int rx = x + 92;
  tprint(rx, 18, 1, CYAN, "Regen");
  char p[12]; snprintf(p, sizeof(p), "%.1f mm", wx.precipSum); tprint(rx, 30, 1, WHITE, p);
  char pp[14]; snprintf(pp, sizeof(pp), "Chance %d%%", wx.precipProb); tprint(rx, 50, 1, LIGHTGREY, pp);
  barH(rx, 62, 60, 8, wx.precipProb / 100.0f * (0.5f + 0.5f * fabsf(sinf(ph * 1.5f))), gfx->color565(0, 120, 230));
}

void cardSun(int x, float ph) {
  header(x, "SONNE");
  int cx = x + SCR_W / 2, cy = 56, r = 34;
  // Bogen
  for (int a = 180; a <= 360; a += 6) {
    float ra = a * DEG_TO_RAD;
    gfx->drawPixel(cx + cosf(ra) * r, cy + sinf(ra) * r, gfx->color565(70, 70, 40));
  }
  gfx->drawFastHLine(cx - r - 4, cy, 2 * r + 8, gfx->color565(50, 50, 60));
  // wandernde Sonne
  float t = fmodf(ph * 0.15f, 1.0f);
  float ang = (180 + t * 180) * DEG_TO_RAD;
  int sx = cx + cosf(ang) * r, sy = cy + sinf(ang) * r;
  iconSun(sx, sy, 5, ph, YELLOW);
  tprint(x + 6, 20, 1, ORANGE, "auf"); tprint(x + 6, 31, 2, WHITE, wx.sunrise);
  int sw = 6 * (int)strlen(wx.sunset) * 2;
  tprint(x + SCR_W - 6 - 18, 20, 1, ORANGE, "unter");
  tprint(x + SCR_W - 6 - sw, 31, 2, WHITE, wx.sunset);
  // Tageslaenge
  int rh = atoi(wx.sunrise) * 60 + atoi(wx.sunrise + 3);
  int sh = atoi(wx.sunset) * 60 + atoi(wx.sunset + 3);
  int dl = sh - rh; if (dl < 0) dl += 1440;
  char d[18]; snprintf(d, sizeof(d), "Tag %dh %02dmin", dl / 60, dl % 60);
  tprint(cx - (int)strlen(d) * 3, 70, 1, LIGHTGREY, d);
}

void cardForecast(int x, float ph) {
  header(x, "3 TAGE");
  for (int i = 0; i < 3; i++) {
    int colx = x + 18 + i * 48;
    gfx->drawFastVLine(x + 50 + i * 48, 18, 54, gfx->color565(35, 35, 45));
    tprint(colx - 6, 18, 1, CYAN, wx.fday[i]);
    drawIconSmall(codeToKind(wx.fcode[i]), colx, 42);
    char mx[6], mn[6];
    snprintf(mx, sizeof(mx), "%d", (int)lroundf(wx.fmax[i]));
    snprintf(mn, sizeof(mn), "%d", (int)lroundf(wx.fmin[i]));
    tprint(colx - 6, 58, 1, ORANGE, mx);
    tprint(colx + 12, 58, 1, CYAN, mn);
  }
}

typedef void (*CardFn)(int, float);
CardFn cards[] = {cardNow, cardWind, cardAtmo, cardToday, cardSun, cardForecast};
const int NUM_CARDS = 6;

void pageDots(int cur) {
  int n = NUM_CARDS, tot = n * 6 - 2, x0 = (SCR_W - tot) / 2;
  for (int i = 0; i < n; i++)
    gfx->fillCircle(x0 + i * 6 + 1, SCR_H - 3, 1, i == cur ? CYAN : gfx->color565(50, 50, 60));
}

// ================= LED-Stimmung =================
void updateLed(float ph) {
  CRGB base;
  switch (codeToKind(wx.wcode)) {
    case WX_CLEAR: base = wx.isDay ? CRGB(255, 170, 30) : CRGB(20, 20, 80); break;
    case WX_FEW: base = CRGB(180, 160, 80); break;
    case WX_CLOUD: case WX_FOG: base = CRGB(80, 90, 110); break;
    case WX_DRIZZLE: case WX_RAIN: base = CRGB(0, 80, 200); break;
    case WX_SNOW: base = CRGB(160, 190, 230); break;
    case WX_THUNDER: base = (fmodf(ph, 2.2f) < 0.18f) ? CRGB(255, 255, 255) : CRGB(120, 60, 200); break;
    default: base = CRGB(60, 60, 60);
  }
  float b = 0.55f + 0.45f * sinf(ph * 1.6f);            // sanftes Atmen
  led[0] = base; led[0].nscale8((uint8_t)(b * 255));
  FastLED.show();
}

// ================= Status / Daten =================
void splash(const char *l1, const char *l2, uint16_t col) {
  gfx->fillScreen(BLACK);
  tprint(8, 24, 2, col, l1);
  if (l2 && l2[0]) tprint(8, 50, 1, LIGHTGREY, l2);
  gfx->flush();
}

bool connectWifi() {
  Serial.printf("Verbinde mit %s ...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long s = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - s < 20000) { delay(300); Serial.print("."); }
  bool ok = WiFi.status() == WL_CONNECTED;
  Serial.println(ok ? ("\nWLAN ok " + WiFi.localIP().toString()) : "\nWLAN FEHLER");
  return ok;
}

void cpyHHMM(char *dst, const char *iso) {  // "...T05:01" -> "05:01"
  if (strlen(iso) >= 16) { strncpy(dst, iso + 11, 5); dst[5] = 0; }
}

bool fetchWeather(Wx &out) {
  if (WiFi.status() != WL_CONNECTED && !connectWifi()) { Serial.println("Kein WLAN"); return false; }

  String url = String("http://api.open-meteo.com/v1/forecast?latitude=") + LAT + "&longitude=" + LON +
    "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,precipitation,weather_code,cloud_cover,pressure_msl,wind_speed_10m,wind_direction_10m,wind_gusts_10m" +
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset,uv_index_max,precipitation_sum,precipitation_probability_max" +
    "&timezone=Europe%2FBerlin&forecast_days=4";

  WiFiClient client; HTTPClient http; http.setTimeout(15000);
  if (!http.begin(client, url)) { Serial.println("http.begin() fehlgeschlagen"); return false; }
  int code = http.GET();
  if (code != 200) { http.end(); Serial.printf("HTTP %d\n", code); return false; }

  StaticJsonDocument<512> filter;
  filter["current"] = true;
  filter["daily"] = true;
  DynamicJsonDocument doc(6144);
  // direkt aus dem Stream parsen: kein zweiter Voll-Puffer (kein getString()) -> weniger RAM/Heap-Fragmentierung
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { Serial.printf("JSON: %s\n", err.c_str()); return false; }

  JsonObject c = doc["current"];
  out.temp = c["temperature_2m"] | 0.0f;
  out.hum = c["relative_humidity_2m"] | 0.0f;
  out.feels = c["apparent_temperature"] | 0.0f;
  out.isDay = c["is_day"] | 1;
  out.precip = c["precipitation"] | 0.0f;
  out.wcode = c["weather_code"] | -1;
  out.cloud = c["cloud_cover"] | 0.0f;
  out.press = c["pressure_msl"] | 0.0f;
  out.wind = c["wind_speed_10m"] | 0.0f;
  out.windDir = c["wind_direction_10m"] | 0;
  out.gust = c["wind_gusts_10m"] | 0.0f;
  cpyHHMM(out.updated, c["time"] | "");

  JsonObject d = doc["daily"];
  out.tmax = d["temperature_2m_max"][0] | 0.0f;
  out.tmin = d["temperature_2m_min"][0] | 0.0f;
  out.uv = d["uv_index_max"][0] | 0.0f;
  out.precipSum = d["precipitation_sum"][0] | 0.0f;
  out.precipProb = d["precipitation_probability_max"][0] | 0;
  cpyHHMM(out.sunrise, d["sunrise"][0] | "");
  cpyHHMM(out.sunset, d["sunset"][0] | "");
  for (int i = 0; i < 3; i++) {
    out.fcode[i] = d["weather_code"][i + 1] | -1;
    out.fmax[i] = d["temperature_2m_max"][i + 1] | 0.0f;
    out.fmin[i] = d["temperature_2m_min"][i + 1] | 0.0f;
    strncpy(out.fday[i], wdayName(d["time"][i + 1] | ""), 3); out.fday[i][3] = 0;
  }

  out.valid = true;
  Serial.printf("OK %.1fC code %d wind %.0f hum %.0f uv %.1f\n", out.temp, out.wcode, out.wind, out.hum, out.uv);
  return true;
}

// ================= Ablauf =================
const unsigned long UPDATE_MS = 10UL * 60UL * 1000UL;
const unsigned long CARD_MS = 5000;       // Anzeigedauer pro Karte
const float SLIDE_SPEED = 3.2f;           // Slide-Tempo
unsigned long lastCard = 0;
int curCard = 0, prevCard = 0;
float slide = 1.0f;                       // 1 = fertig, <1 waehrend Uebergang

// Laeuft eigenstaendig auf Core 0: holt das Wetter und legt es im Staging-Puffer ab.
// Beruehrt NIE das Display/gfx (gehoert allein dem loop() auf Core 1).
void weatherTask(void *pv) {
  for (;;) {
    Wx tmp;                                 // frische lokale Kopie (Defaultwerte aus Wx)
    bool ok = fetchWeather(tmp);
    if (ok) {
      xSemaphoreTake(wxMutex, portMAX_DELAY);
      wxNew = tmp; wxReady = true;
      xSemaphoreGive(wxMutex);
    }
    online = ok;
    vTaskDelay(pdMS_TO_TICKS(ok ? UPDATE_MS : 15000));   // bei Fehler frueher erneut versuchen
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(TFT_LEDA, OUTPUT); digitalWrite(TFT_LEDA, LOW);   // Backlight an
  gfx->begin(40000000);
  FastLED.addLeds<APA102, LED_DI, LED_CI, BGR>(led, 1);
  FastLED.setBrightness(60);
  splash("Wetter", "Dongle DELUXE", WHITE); delay(900);

  wxMutex = xSemaphoreCreateMutex();
  // Wetterabruf auf Core 0 auslagern -> Animation/LED bleiben auf Core 1 fluessig
  xTaskCreatePinnedToCore(weatherTask, "weather", 8192, nullptr, 1, nullptr, 0);

  lastCard = millis();
}

void loop() {
  unsigned long now = millis();

  // frische Wetterdaten vom Hintergrund-Task uebernehmen (zwischen den Frames -> kein Tearing)
  if (wxReady) {
    xSemaphoreTake(wxMutex, portMAX_DELAY);
    wx = wxNew; wxReady = false;
    xSemaphoreGive(wxMutex);
  }

  // solange noch keine Daten da sind: Ladehinweis statt Carousel
  if (!wx.valid) {
    splash("Wetter", online ? "lade Daten..." : "verbinde...", CYAN);
    delay(100);
    return;
  }

  // Kartenwechsel ausloesen
  if (slide >= 1.0f && now - lastCard >= CARD_MS) {
    prevCard = curCard; curCard = (curCard + 1) % NUM_CARDS; slide = 0.0f; lastCard = now;
  }

  float ph = now / 1000.0f;
  if (slide < 1.0f) slide += SLIDE_SPEED * 0.033f;
  if (slide > 1.0f) slide = 1.0f;
  float e = 1 - powf(1 - slide, 3);      // ease-out

  gfx->fillScreen(BLACK);
  if (slide < 1.0f) {
    int off = (int)(e * SCR_W);
    cards[prevCard](-off, ph);
    cards[curCard](SCR_W - off, ph);
  } else {
    cards[curCard](0, ph);
  }
  pageDots(curCard);
  if (!online) gfx->fillCircle(SCR_W - 3, 3, 1, RED);   // letzter Abruf fehlgeschlagen / kein Netz
  gfx->flush();

  updateLed(ph);
  delay(20);   // ~30 fps
}
