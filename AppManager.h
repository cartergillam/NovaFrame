#pragma once

#include <Arduino.h>
#include <vector>
#include "BaseApp.h"

class AppManager {
public:
  void init();          // Load enabled apps from Firebase
  void loop();          // Handle app switching and rendering

  BaseApp* getActiveApp();  // Get the currently active app

private:
  void nextApp();                   // Advance to next app in sequence
  void loadApp(const String& appId); // Load app by ID

  std::vector<String> enabledApps;  // App IDs from Firebase
  int currentIndex = 0;
  unsigned long lastSwitchTime = 0;
  const unsigned long appDuration = 10000; // 10 seconds per app
  BaseApp* currentApp = nullptr;
};