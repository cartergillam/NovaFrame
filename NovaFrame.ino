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

#define BUTTON_PIN A1
#define HOLD_TIME 2000

AppManager appManager;
ClockApp clockApp;
ClockWeatherApp clockWeatherApp;
WeatherApp weatherApp;
TimeCache timeCache;
ForecastApp forecastApp;

std::map<String, BaseApp*> appRegistry;

WiFiManager wm;
unsigned long buttonPressStart = 0;
bool buttonHeld = false;
bool isUpdating = false;
unsigned long lastOTACheck = 0;
const unsigned long OTA_INTERVAL = 60 * 60 * 1000;

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
  appRegistry["forecast"] = &forecastApp;

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
  if (millis() - lastOTACheck > OTA_INTERVAL) {
    checkForOTAUpdate();
    lastOTACheck = millis();
  }
}
