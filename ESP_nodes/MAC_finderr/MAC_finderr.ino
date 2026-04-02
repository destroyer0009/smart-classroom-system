#include <WiFi.h>

void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);   // ✅ REQUIRED
  WiFi.disconnect();     // ✅ Reset WiFi

  delay(100);

  Serial.println("MAC Address:");
  Serial.println(WiFi.macAddress());
}

void loop() {}