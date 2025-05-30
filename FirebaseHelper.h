#pragma once

#include <Arduino.h>
#include <vector>

// Returns enabled apps in order from Firebase, e.g. ["weather", "clockWeather"]
bool getEnabledAppsFromFirebase(std::vector<String>& enabledApps, bool forceRefresh);
bool fetchAppSequenceFromFirebase(std::vector<String>& sequence, bool forceRefresh);
bool setAppSequenceToFirebase(const std::vector<String>& sequence);