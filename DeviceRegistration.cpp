#include "DeviceRegistration.h"
#include "DisplayHelpers.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "SecretsManager.h"
#include <LittleFS.h>
#include "RemoteConfigManager.h"
#include "DeviceConfig.h"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

String units = "metric";
String deviceID = "";
int timeFormatPreference = 1;
int lastTimeFormat = timeFormatPreference;
float storedLat = 0.0;
float storedLon = 0.0;
String deviceTimezone = "";

String getSanitizedMac() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  return mac;
}

void initializeWiFi() {
  wm.setConnectTimeout(WIFI_TIMEOUT);
  setupCustomWiFiManager(wm);

  Serial.println("Attempting connection using saved credentials...");
  showConnectingToWiFi();

  WiFi.disconnect(true);
  delay(100);
  WiFi.begin();

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(200);
  }

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);
    Serial.println("✅ WiFi connected successfully.");
    Serial.println("📶 WiFi modem sleep disabled for socket stability.");
    showWifiInfo();
    delay(2000);
    return;
  }

  Serial.println("⚠️ Saved WiFi failed. Launching setup portal...");
  showWifiNotSetNotice();
  delay(2000);
  showJoinInstructions();
  delay(3000);

  bool connected = wm.startConfigPortal("NovaFrame-Setup");
  if (connected) {
    WiFi.setSleep(false);
    Serial.println("✅ Connected via portal.");
    Serial.println("📶 WiFi modem sleep disabled for socket stability.");
    matrix.fillScreen(0);
    showCenteredText("Connected!", 10, matrix.color565(0, 255, 0));
    matrix.show();
    delay(1500);
    showWifiInfo();
    delay(2000);
  } else {
    Serial.println("❌ Portal failed. Resetting credentials.");
    wm.resetSettings();
    matrix.fillScreen(0);
    showCenteredText("WiFi Fail", 10, matrix.color565(255, 0, 0));
    matrix.show();
    delay(2000);
  }
}

void initializeFirebase() {
  if (!SecretsManager::load()) {
    Serial.println("❌ Could not load secrets");
    return;
  }

  config.api_key = std::string(SecretsManager::get("FIREBASE_API_KEY").c_str());
  config.database_url = std::string(SecretsManager::get("FIREBASE_HOST").c_str());
  auth.user.email = std::string(SecretsManager::get("FIREBASE_EMAIL").c_str());
  auth.user.password = std::string(SecretsManager::get("FIREBASE_PASSWORD").c_str());

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  Serial.println("Waiting for Firebase token...");
  while (!Firebase.ready()) delay(100);
  deviceID = getSanitizedMac();
  Serial.println("📟 deviceID set to: " + deviceID);
  RemoteConfigManager::begin();
}

void updateGeoLocationAndTimezone(const String& settingsPath) {
  (void)settingsPath;
  initializeDeviceConfig(true);
  refreshDeviceConfig(true);
  deviceTimezone = deviceSettings.timezone;
}

void registerDeviceInFirebase(bool allowLocationLookup) {
  Serial.println("📝 Registering device in Firebase...");

  if (!Firebase.ready()) {
    Serial.println("❌ Firebase not ready — skipping registration.");
    return;
  }

  if (!initializeDeviceConfig(allowLocationLookup)) {
    Serial.println("❌ Failed to initialize per-device config.");
    return;
  }

  brightnessLevel = deviceSettings.brightness;
  units = deviceSettings.units;
  timeFormatPreference = deviceSettings.timeFormat;
  storedLat = deviceSettings.locationLat;
  storedLon = deviceSettings.locationLon;
  deviceTimezone = deviceSettings.timezone;
  Serial.println("📍 Per-device settings initialized or updated.");
}

bool loadSecretsFromFlash() {
  if (!LittleFS.begin()) {
    Serial.println("❌ Failed to mount LittleFS");
    return false;
  }

  File file = LittleFS.open("/secrets.json", "r");
  if (!file) {
    Serial.println("❌ secrets.json not found");
    return false;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    Serial.println("❌ Failed to parse secrets.json");
    return false;
  }

  config.api_key = std::string(doc["FIREBASE_API_KEY"].as<String>().c_str());
  auth.user.email = std::string(doc["FIREBASE_EMAIL"].as<String>().c_str());
  auth.user.password = std::string(doc["FIREBASE_PASSWORD"].as<String>().c_str());
  return true;
}

String getDeviceID() {
  return deviceID;
}
