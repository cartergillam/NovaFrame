#pragma once

#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <WiFiClient.h>
#include "DisplayHelpers.h"
#include "SecretsManager.h"

extern Adafruit_Protomatter matrix;
extern bool isUpdating;

void checkForOTAUpdate() {
  isUpdating = true;

  String currentVersion = SecretsManager::get("CURRENT_VERSION");
  String otaJsonUrl = SecretsManager::get("OTA_JSON_URL");

  if (currentVersion == "" || otaJsonUrl == "") {
    Serial.println("❌ Missing CURRENT_VERSION or OTA_JSON_URL in secrets.");
    isUpdating = false;
    return;
  }

  HTTPClient http;
  http.begin(otaJsonUrl);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, payload);

    if (error || !doc.containsKey("version") || !doc.containsKey("url")) {
      Serial.println("❌ Invalid or missing keys in version.json");
      isUpdating = false;
      http.end();
      return;
    }

    String newVersion = doc["version"];
    String firmwareURL = doc["url"];

    if (newVersion != currentVersion) {
      Serial.println("🔁 New firmware available: " + newVersion);
      Serial.println("⬇️ Firmware URL: " + firmwareURL);

      // ⚠️ On-screen warning
      matrix.fillScreen(0);
      showCenteredText("Updating", 2, matrix.color565(255, 255, 255));
      showCenteredText("Do Not", 12, matrix.color565(255, 0, 0));
      showCenteredText("Unplug", 22, matrix.color565(255, 0, 0));
      matrix.show();
      delay(3000);

      matrix.fillScreen(0);
      matrix.show();

      http.end();
      http.begin(firmwareURL);
      int code = http.GET();

      if (code == 200) {
        int contentLen = http.getSize();
        WiFiClient* stream = http.getStreamPtr();

        if (Update.begin(contentLen)) {
          size_t written = Update.writeStream(*stream);

          if (written == contentLen && Update.end()) {
            Serial.println("✅ OTA Update complete. Rebooting...");
            SecretsManager::set("CURRENT_VERSION", newVersion);
            matrix.fillScreen(0);
            matrix.show();
            delay(200);
            ESP.restart();
          } else {
            Serial.println("❌ OTA write failed or incomplete.");
            Update.abort();
          }
        } else {
          Serial.println("❌ Not enough space for OTA.");
        }
      } else {
        Serial.println("❌ Failed to fetch firmware: " + http.errorToString(code));
      }

      http.end();
    } else {
      Serial.println("✅ Firmware is already up to date.");
      http.end();
    }
  } else {
    Serial.println("❌ Failed to check version.json: " + http.errorToString(httpCode));
    http.end();
  }

  isUpdating = false;
}