#pragma once

#include <Arduino.h>
#include <vector>
#include "BaseApp.h"

class AppManager {
public:
  void init();          // Load enabled apps from Firebase
  void loop();          // Handle app switching and rendering

  BaseApp* getActiveApp();  // Get the currently active app
  String getActiveAppId() const;

private:
  void applyConfigChanges(uint32_t changeFlags);
  void nextApp();
  void loadApp(const String& appId);
  void rebuildRuntimeApps(bool forceReload);
  bool shouldSleep() const;
  void blankDisplay();

  std::vector<String> runtimeApps;
  int currentIndex = 0;
  unsigned long lastSwitchTime = 0;
  unsigned long currentDurationMs = 10000;
  BaseApp* currentApp = nullptr;
  String currentAppId = "";
  bool sleeping = false;
};
