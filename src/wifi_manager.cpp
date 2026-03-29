#include "wifi_manager.h"

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"

namespace robot {

bool WiFiManager::connectIfNeeded() {
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint8_t retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 30) {
    delay(500);
    Serial.print('.');
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("\nWiFi connection failed");
  return false;
}

}  // namespace robot
