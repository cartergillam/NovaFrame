#include "DeviceRegistration.h"
#include "DisplayHelpers.h"
#include "BaseApp.h"
#include "ClockWeatherApp.h"
#include "ClockApp.h"
#include "WeatherApp.h"
#include "AppManager.h"
#include "WiFiPortalCustomizer.h"
#include "WeatherCache.h"
#include "TimeCache.h"
#include "secrets.h"
#include <HTTPClient.h>
#include <Update.h>
#include <map>

#define CURRENT_VERSION "1.0.2"
#define BUTTON_PIN A1
#define HOLD_TIME 2000

AppManager appManager;
ClockApp clockApp;
ClockWeatherApp clockWeatherApp;
WeatherApp weatherApp;
TimeCache timeCache;

std::map<String, BaseApp*> appRegistry;

WiFiManager wm;
unsigned long buttonPressStart = 0;
bool buttonHeld = false;
bool isUpdating = false;

void setup() {
  Serial.begin(115200);
  delay(150);
  Wire.begin();
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  initializeDisplay();
  showWelcome();
  initializeWiFi();
  initializeFirebase();

  appRegistry["clock"] = &clockApp;
  appRegistry["clockWeather"] = &clockWeatherApp;
  appRegistry["weather"] = &weatherApp;

  registerDeviceInFirebase();
  updateWeatherCache();
  timeCache.init();
  showWifiInfo();
  delay(2000);
  matrix.fillScreen(0);

  while (!Firebase.ready()) {
    Serial.println("⏳ Waiting for Firebase to be ready...");
    delay(100);
  }

  checkForOTAUpdate();
  delay(2000);
  appManager.init();
  matrix.show();
}

void loop() {
  if (isUpdating) return;

  bool buttonDown = digitalRead(BUTTON_PIN) == LOW;
  unsigned long now = millis();

  if (buttonDown) {
    if (buttonPressStart == 0) {
      buttonPressStart = now;
    } else if (!buttonHeld && (now - buttonPressStart >= HOLD_TIME)) {
      buttonHeld = true;
      matrix.fillScreen(0);
      showCenteredText("Reset WiFi", 12, matrix.color565(255, 0, 0));
      matrix.show();
      wm.resetSettings();
      delay(1000);
      ESP.restart();
    }
  } else {
    buttonPressStart = 0;
    buttonHeld = false;
  }

  static unsigned long lastGlobalBrightnessCheck = 0;
  if (now - lastGlobalBrightnessCheck > 5000) {
    int prevLevel = brightnessLevel;
    int prevTimeFormat = timeFormatPreference;
    String prevUnits = getUnits();  // Ensure getter exists

    checkBrightnessUpdate();
    checkTimeFormatUpdate();
    checkUnitsUpdate();

    BaseApp* current = appManager.getActiveApp();
    if (current && (
          brightnessLevel != prevLevel ||
          timeFormatPreference != prevTimeFormat ||
          getUnits() != prevUnits)) {
      current->setNeedsRedraw(true);
    }

    lastGlobalBrightnessCheck = now;
  }

  BaseApp* current = appManager.getActiveApp();
  if (current) {
    appManager.loop();

    if (current->getNeedsRedraw()) {
      current->redraw(true);
      current->setNeedsRedraw(false);
    }
  }

  delay(100);
}

void checkForOTAUpdate() {
  isUpdating = true;
  HTTPClient http;
  http.begin("https://cartergillam.github.io/novaframe-firmware/version.json");
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

    if (newVersion != CURRENT_VERSION) {
      matrix.fillScreen(0);
      showCenteredText("Updating", 2, matrix.color565(255, 255, 255));
      showCenteredText("Do Not", 12, matrix.color565(255, 255, 255));
      showCenteredText("Unplug", 22, matrix.color565(255, 255, 255));
      matrix.show();

      unsigned long waitStart = millis();
      while (millis() - waitStart < 3000) yield();

      matrix.fillScreen(0);
      matrix.show();

      Serial.println("🔁 New version available: " + newVersion);
      Serial.println("⬇️ Downloading update...");

      http.end();
      http.begin(firmwareURL);
      int code = http.GET();

      if (code == 200) {
        int contentLen = http.getSize();
        if (Update.begin(contentLen)) {
          WiFiClient* stream = http.getStreamPtr();
          size_t written = Update.writeStream(*stream);

          if (written == contentLen && Update.end()) {
            Serial.println("✅ OTA Update complete. Rebooting...");
            matrix.fillScreen(0);
            matrix.show();
            delay(200);
            ESP.restart();
          } else {
            Serial.println("❌ OTA write failed or incomplete");
          }
        } else {
          Serial.println("❌ Not enough space for OTA");
        }
      } else {
        Serial.println("❌ Failed to fetch firmware: " + http.errorToString(code));
      }

      http.end();
    } else {
      Serial.println("✅ Firmware is up to date.");
      http.end();
    }
  } else {
    Serial.println("❌ Failed to check version.json: " + http.errorToString(httpCode));
    http.end();
  }

  isUpdating = false;
}