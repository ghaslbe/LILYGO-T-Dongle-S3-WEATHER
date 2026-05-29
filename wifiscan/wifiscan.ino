#include <WiFi.h>

const char *encStr(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    default:                        return "?";
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(200);
}

void loop() {
  Serial.println("\n=== SCAN START ===");
  int n = WiFi.scanNetworks();
  Serial.printf("Gefundene Netze: %d\n", n);
  for (int i = 0; i < n; i++) {
    Serial.printf("%2d) RSSI %4d dBm  Ch %2d  %-10s  \"%s\"\n",
                  i, WiFi.RSSI(i), WiFi.channel(i),
                  encStr(WiFi.encryptionType(i)), WiFi.SSID(i).c_str());
  }
  Serial.println("=== SCAN ENDE ===");
  delay(8000);
}
