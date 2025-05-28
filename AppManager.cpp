#include "AppManager.h"
#include "FirebaseHelper.h"
#include <Firebase_ESP_Client.h>
#include "DeviceRegistration.h"
#include "TimeCache.h"
#include <map>
#include <vector>
#include <ArduinoJson.h>

extern FirebaseData fbdo;
extern FirebaseAuth auth;
extern FirebaseConfig config;
extern std::map<String, BaseApp*> appRegistry;
extern String deviceID;
extern TimeCache timeCache;

void AppManager::init() {
  std::vector<String> loadedApps;
  if (getEnabledAppsFromFirebase(loadedApps)) {
    std::vector<String> validApps;
    for (const auto& app : loadedApps) {
      if (app.length() > 0 && appRegistry.count(app)) {
        validApps.push_back(app);
      } else {
        Serial.println("⚠️ Skipping invalid app ID: " + app);
      }
    }
    enabledApps = validApps;
  }

  if (enabledApps.empty()) {
    Serial.println("⚠️ No valid apps enabled. Falling back to clock.");
    enabledApps.push_back("clock");
  }

  currentIndex = 0;
  loadApp(enabledApps[currentIndex]);
  lastSwitchTime = millis();
}

void AppManager::loop() {
  unsigned long now = millis();

  if (enabledApps.size() >= 2 && now - lastSwitchTime >= appDuration) {
    Serial.println("⏭️ Switching to next app...");
    nextApp();
    lastSwitchTime = now;
  }

  if (currentApp) {
    timeCache.updateIfNeeded();
    currentApp->loop();

    if (currentApp->getNeedsRedraw()) {
      currentApp->redraw(true);
      currentApp->setNeedsRedraw(false);
    }
  } else {
    Serial.println("❌ currentApp is NULL");
  }

  static unsigned long lastPoll = 0;
  if (now - lastPoll >= 10000) {
    lastPoll = now;
    checkBrightnessUpdate();
    checkTimeFormatUpdate();
    checkUnitsUpdate();

    std::vector<String> updatedApps;
    if (getEnabledAppsFromFirebase(updatedApps)) {
      std::vector<String> validApps;
      for (const auto& app : updatedApps) {
        if (app.length() > 0 && appRegistry.count(app)) {
          validApps.push_back(app);
        } else {
          Serial.println("⚠️ Skipping unregistered or empty app ID: " + app);
        }
      }

      if (validApps != enabledApps) {
        Serial.println("🔁 Firebase appSequence changed. Reloading apps.");
        enabledApps = validApps;

        if (enabledApps.empty()) {
          enabledApps.push_back("clock");
          Serial.println("⚠️ No valid apps after update. Using fallback: clock.");
        }

        currentIndex = 0;
        loadApp(enabledApps[currentIndex]);
        lastSwitchTime = now;

        String appSeqPath = "/novaFrame/devices/" + deviceID + "/settings/appSequence";
        FirebaseJsonArray jsonArray;
        for (const auto& appId : enabledApps) {
          jsonArray.add(appId);
        }

        if (!Firebase.RTDB.setArray(&fbdo, appSeqPath.c_str(), &jsonArray)) {
          Serial.print("❌ Failed to update appSequence: ");
          Serial.println(fbdo.errorReason());
        } else {
          Serial.println("✅ Updated appSequence in Firebase.");
        }
      }
    } else {
      Serial.println("❌ Failed to fetch appSequence from Firebase");
    }
  }
}

void AppManager::nextApp() {
  if (enabledApps.empty()) return;
  currentIndex = (currentIndex + 1) % enabledApps.size();
  loadApp(enabledApps[currentIndex]);
}

void AppManager::loadApp(const String& appId) {
  if (appId.length() == 0) {
    Serial.println("⚠️ Ignoring empty app ID.");
    return;
  }

  if (appRegistry.count(appId)) {
    matrix.fillScreen(0);
    matrix.show();

    currentApp = appRegistry[appId];
    currentApp->init();
    currentApp->setNeedsRedraw(true);
    currentApp->redraw(true);

    Serial.println("✅ App loaded and redrawn: " + appId);
  } else {
    Serial.print("❌ App not registered: ");
    Serial.println(appId);
  }
}

BaseApp* AppManager::getActiveApp() {
  return currentApp;
}