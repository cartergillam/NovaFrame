#include "AppManager.h"
#include <Firebase_ESP_Client.h>
#include "DeviceRegistration.h"
#include "DeviceConfig.h"
#include "DisplayHelpers.h"
#include "TimeCache.h"
#include "WeatherCache.h"
#include <algorithm>
#include <map>
#include <vector>

extern FirebaseData fbdo;
extern FirebaseAuth auth;
extern FirebaseConfig config;
extern std::map<String, BaseApp*> appRegistry;
extern TimeCache timeCache;
extern AppManager appManager;
extern int brightnessLevel;
extern unsigned long lastWeatherFetchTime;

namespace {

const char* kCanonicalRuntimeOrder[] = {
  "clockWeather", "forecast", "stocks", "mlb", "nba", "nfl", "nhl", "clock", "weather"
};

}

void AppManager::init() {
  refreshDeviceConfig(true);
  maybeAutoUpdateLocationFromGeo(true);
  brightnessLevel = deviceSettings.brightness;
  updateWeatherCache();
  timeCache.init();
  rebuildRuntimeApps(true);
  takeDeviceConfigChangeFlags();
  lastSwitchTime = millis();
}

void AppManager::loop() {
  unsigned long now = millis();

  pollDeviceConfigStreams();
  maybeAutoUpdateLocationFromGeo(false);
  applyConfigChanges(takeDeviceConfigChangeFlags());
  updateWeatherCache();
  timeCache.updateIfNeeded();

  bool shouldSleepNow = shouldSleep();
  if (shouldSleepNow != sleeping) {
    sleeping = shouldSleepNow;
    if (sleeping) {
      blankDisplay();
    } else if (currentApp) {
      currentApp->setNeedsRedraw(true);
    }
  }

  if (sleeping) {
    writeDeviceStatus(currentAppId, true, timeCache.getCurrentUnixTime(), timeCache.isSynchronized());
    return;
  }

  if (runtimeApps.size() >= 2 && now - lastSwitchTime >= currentDurationMs) {
    Serial.println("⏭️ Switching to next app...");
    nextApp();
    lastSwitchTime = now;
  }

  if (currentApp) {
    currentApp->loop();

    if (currentApp->getNeedsRedraw()) {
      currentApp->redraw(true);
      currentApp->setNeedsRedraw(false);
      matrix.show();
    }
  } else {
    Serial.println("❌ currentApp is NULL");
  }
  writeDeviceStatus(currentAppId, false, timeCache.getCurrentUnixTime(), timeCache.isSynchronized());
}

void AppManager::applyConfigChanges(uint32_t changeFlags) {
  if (changeFlags == DeviceConfigChangeNone) {
    return;
  }

  bool needsRedraw = false;

  if (changeFlags & DeviceConfigChangeBrightness) {
    int previousBrightness = brightnessLevel;
    brightnessLevel = deviceSettings.brightness;
    Serial.printf("🔆 Brightness update: %d -> %d\n", previousBrightness, brightnessLevel);
    needsRedraw = true;
  }

  if (changeFlags & DeviceConfigChangeTimeFormat) {
    needsRedraw = true;
  }

  if (changeFlags & DeviceConfigChangeUnits) {
    lastWeatherFetchTime = 0;
    updateWeatherCache();
    needsRedraw = true;
  }

  if (changeFlags & DeviceConfigChangeLocation) {
    lastWeatherFetchTime = 0;
    timeCache.init();
    updateWeatherCache();
    needsRedraw = true;
  }

  if (changeFlags & DeviceConfigChangeSleepSchedule) {
    needsRedraw = true;
  }

  if (changeFlags & (DeviceConfigChangeSequence | DeviceConfigChangeAppState)) {
    rebuildRuntimeApps(false);
  }

  if ((changeFlags & DeviceConfigChangeStocks) && currentAppId == "stocks") {
    needsRedraw = true;
  }

  if ((changeFlags & DeviceConfigChangeSports) &&
      (currentAppId == "mlb" || currentAppId == "nba" || currentAppId == "nfl" || currentAppId == "nhl")) {
    needsRedraw = true;
  }

  if (needsRedraw && currentApp != nullptr) {
    currentApp->setNeedsRedraw(true);
  }
}

void AppManager::nextApp() {
  if (runtimeApps.empty()) return;
  currentIndex = (currentIndex + 1) % runtimeApps.size();
  loadApp(runtimeApps[currentIndex]);
}

void AppManager::loadApp(const String& appId) {
  if (appId.length() == 0) {
    Serial.println("⚠️ Ignoring empty app ID.");
    return;
  }

  if (appRegistry.count(appId)) {
    matrix.fillScreen(0);

    currentApp = appRegistry[appId];
    currentAppId = appId;
    currentDurationMs = getAppDurationMs(appId);
    currentApp->init();
    currentApp->setNeedsRedraw(true);

    Serial.println("✅ App loaded and redrawn: " + appId);
  } else {
    Serial.print("❌ App not registered: ");
    Serial.println(appId);
  }
}

BaseApp* AppManager::getActiveApp() {
  return currentApp;
}

String AppManager::getActiveAppId() const {
  return currentAppId;
}

void AppManager::rebuildRuntimeApps(bool forceReload) {
  std::vector<String> nextApps;

  for (const auto& appId : deviceSettings.appSequence) {
    if (appId.length() == 0) continue;
    if (!appRegistry.count(appId)) {
      Serial.println("⚠️ Skipping unregistered app in sequence: " + appId);
      continue;
    }
    if (!isAppEnabled(appId)) continue;
    if (std::find(nextApps.begin(), nextApps.end(), appId) == nextApps.end()) {
      nextApps.push_back(appId);
    }
  }

  for (const char* rawId : kCanonicalRuntimeOrder) {
    String appId(rawId);
    if (!appRegistry.count(appId)) continue;
    if (!isAppEnabled(appId)) continue;
    if (std::find(nextApps.begin(), nextApps.end(), appId) == nextApps.end()) {
      nextApps.push_back(appId);
    }
  }

  for (const auto& entry : deviceAppConfigs) {
    if (!appRegistry.count(entry.first)) continue;
    if (!entry.second.enabled) continue;
    if (std::find(nextApps.begin(), nextApps.end(), entry.first) == nextApps.end()) {
      nextApps.push_back(entry.first);
    }
  }

  if (nextApps.empty()) {
    if (appRegistry.count("clockWeather")) {
      nextApps.push_back("clockWeather");
    } else if (appRegistry.count("clock")) {
      nextApps.push_back("clock");
    }
  }

  if (nextApps.empty()) return;

  bool sequenceChanged = nextApps != runtimeApps;
  runtimeApps = nextApps;

  auto activeIt = std::find(runtimeApps.begin(), runtimeApps.end(), currentAppId);
  bool activeStillPresent = activeIt != runtimeApps.end();
  if (!activeStillPresent) {
    currentIndex = 0;
    loadApp(runtimeApps[currentIndex]);
    lastSwitchTime = millis();
    return;
  }

  currentIndex = static_cast<int>(std::distance(runtimeApps.begin(), activeIt));
  unsigned long nextDuration = getAppDurationMs(currentAppId);
  bool durationChanged = nextDuration != currentDurationMs;
  currentDurationMs = nextDuration;

  if (forceReload || sequenceChanged || durationChanged) {
    if (currentApp != nullptr) currentApp->setNeedsRedraw(true);
  }
}

bool AppManager::shouldSleep() const {
  if (!timeCache.isSynchronized()) return false;
  return shouldSleepNow(timeCache.getWeekdayIndex(), timeCache.getMinutesSinceMidnight());
}

void AppManager::blankDisplay() {
  matrix.fillScreen(0);
  matrix.show();
}

// Global helper that delegates to the AppManager instance
BaseApp* getActiveApp() {
  return appManager.getActiveApp();
}
