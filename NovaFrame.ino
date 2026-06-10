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
#include <HTTPClient.h>
#include <Update.h>
#include <map>
#include "OTAUpdater.h"
#include "RemoteConfigManager.h"
#include "ForecastApp.h"
#include "StocksApp.h"
#include "SportsApp.h"
#include "esp_system.h"

#define BUTTON_PIN A1
#define HOLD_TIME 2000

AppManager appManager;
ClockApp clockApp;
ClockWeatherApp clockWeatherApp;
WeatherApp weatherApp;
TimeCache timeCache;
ForecastApp forecastApp;
StocksApp stocksApp;
SportsApp mlbApp("mlb", "MLB");
SportsApp nbaApp("nba", "NBA");
SportsApp nflApp("nfl", "NFL");
SportsApp nhlApp("nhl", "NHL");

std::map<String, BaseApp*> appRegistry;

WiFiManager wm;
unsigned long buttonPressStart = 0;
bool buttonHeld = false;
bool isUpdating = false;
unsigned long lastOTACheck = 0;
const unsigned long OTA_INTERVAL = 60 * 60 * 1000;

const char* resetReasonToString(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
  }
}

void setup() {
  Serial.begin(115200);
  delay(150);
  esp_reset_reason_t resetReason = esp_reset_reason();
  Serial.printf("🔁 Reset reason: %s (%d)\n", resetReasonToString(resetReason), static_cast<int>(resetReason));

  Wire.begin();
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // 🔧 Initialize display and Wi-Fi
  initializeDisplay();
  showWelcome();
  initializeWiFi();

  // 🌐 Connect to Firebase
  initializeFirebase();

  // 📱 Register device and seed/migrate per-device config.
  registerDeviceInFirebase(true);

  // 📦 Initialize app registry
  appRegistry["clock"] = &clockApp;
  appRegistry["clockWeather"] = &clockWeatherApp;
  appRegistry["weather"] = &weatherApp;
  appRegistry["forecast"] = &forecastApp;
  appRegistry["stocks"] = &stocksApp;
  appRegistry["mlb"] = &mlbApp;
  appRegistry["nba"] = &nbaApp;
  appRegistry["nfl"] = &nflApp;
  appRegistry["nhl"] = &nhlApp;

  showWifiInfo();
  delay(2000);
  matrix.fillScreen(0);

  // ✅ Wait for Firebase to be fully ready before OTA/app manager
  while (!Firebase.ready()) {
    Serial.println("⏳ Waiting for Firebase to be ready...");
    delay(100);
  }

  // 🔁 Check for OTA update (optional, runs in foreground)
  checkForOTAUpdate();
  delay(2000);  // Optional: reduce if OTA is fast

  // 🚀 Start app rotation
  appManager.init();
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

  appManager.loop();

  delay(100);
  if (millis() - lastOTACheck > OTA_INTERVAL) {
    checkForOTAUpdate();
    lastOTACheck = millis();
  }
}
