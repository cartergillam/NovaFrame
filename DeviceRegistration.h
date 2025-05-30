#pragma once

#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <Firebase_ESP_Client.h>
#include <ArduinoJson.h>
#include "WiFiPortalCustomizer.h"
#include "DisplayHelpers.h"
#include <Arduino.h>

#define WIFI_TIMEOUT 15

extern FirebaseData fbdo;
extern FirebaseAuth auth;
extern FirebaseConfig config;
extern WiFiManager wm;
extern String deviceID;

extern String units;              // "metric" or "imperial"
extern int timeFormatPreference; // 0 = 12h + AM/PM, 1 = 12h no suffix, 2 = 24h
extern float storedLat;
extern float storedLon;

void initializeWiFi();
void initializeFirebase();
void registerDeviceInFirebase(bool deferGeo); 
void updateGeoLocationAndTimezone(const String& settingsPath); 
bool loadSecretsFromFlash();
String getSanitizedMac();
void fetchAndStoreTimezone(float lat, float lon);
String getDeviceID(); 