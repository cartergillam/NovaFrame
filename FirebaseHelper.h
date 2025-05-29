#pragma once

#include <Arduino.h>
#include <vector>

// Returns enabled apps in order from Firebase, e.g. ["weather", "clockWeather"]
bool getEnabledAppsFromFirebase(std::vector<String>& apps);
bool fetchAppSequenceFromFirebase(std::vector<String>& sequence);
bool setAppSequenceToFirebase(const std::vector<String>& sequence);