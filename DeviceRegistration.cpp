#include "DeviceRegistration.h"
#include "DisplayHelpers.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "SecretsManager.h"
#include <LittleFS.h>
#include "RemoteConfigManager.h"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

String units = "metric";
String deviceID = "";
int timeFormatPreference = 0;
int lastTimeFormat = timeFormatPreference;
float storedLat = 0.0;
float storedLon = 0.0;

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
    Serial.println("✅ WiFi connected successfully.");
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
    Serial.println("✅ Connected via portal.");
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
  while (!Firebase.ready()) delay(100);

  float currentLat = 0.0;
  float currentLon = 0.0;

  Firebase.RTDB.getFloat(&fbdo, (settingsPath + "/lat").c_str());
  currentLat = fbdo.floatData();

  Firebase.RTDB.getFloat(&fbdo, (settingsPath + "/lon").c_str());
  currentLon = fbdo.floatData();

  HTTPClient geoHttp;
  geoHttp.begin("http://ip-api.com/json");
  int code = geoHttp.GET();
  if (code != 200) {
    Serial.println("❌ GeoIP failed: " + geoHttp.errorToString(code));
    geoHttp.end();
    return;
  }

  String payload = geoHttp.getString();
  geoHttp.end();

  DynamicJsonDocument geoDoc(1024);
  deserializeJson(geoDoc, payload);
  float newLat = geoDoc["lat"];
  float newLon = geoDoc["lon"];
  String city = geoDoc["city"] | "";
  String region = geoDoc["region"] | "";

  bool shouldUpdate = fabs(currentLat - newLat) > 0.01 || fabs(currentLon - newLon) > 0.01;

  if (shouldUpdate) {
    Serial.printf("🌍 Location changed — updating Firebase: (%.4f, %.4f) → (%.4f, %.4f)\n",
                  currentLat, currentLon, newLat, newLon);
    Firebase.RTDB.setString(&fbdo, (settingsPath + "/weatherLocation").c_str(), city + "," + region);
    Firebase.RTDB.setFloat(&fbdo, (settingsPath + "/lat").c_str(), newLat);
    Firebase.RTDB.setFloat(&fbdo, (settingsPath + "/lon").c_str(), newLon);
    storedLat = newLat;
    storedLon = newLon;
  } else {
    Serial.println("📍 Location unchanged — skipping Firebase update.");
    storedLat = currentLat;
    storedLon = currentLon;
  }
}

void registerDeviceInFirebase(bool deferGeo) {
  Serial.println("📝 Registering device in Firebase...");

  if (!Firebase.ready()) {
    Serial.println("❌ Firebase not ready — skipping registration.");
    return;
  }

  String devicePath = "/novaFrame/devices/" + deviceID;
  String settingsPath = devicePath + "/settings";
  String appSeqPath = settingsPath + "/appSequence";
  String appsPath = devicePath + "/apps";

  if (!Firebase.RTDB.getJSON(&fbdo, settingsPath.c_str())) {
    Serial.println("📁 Settings node missing, creating empty shell.");
    FirebaseJson patch;
    patch.set("placeholder", true);
    Firebase.RTDB.updateNode(&fbdo, settingsPath.c_str(), &patch);
  }

  String clockAppPath = appsPath + "/clock";
  if (!Firebase.RTDB.getJSON(&fbdo, clockAppPath.c_str())) {
    FirebaseJson defaultAppConfig;
    defaultAppConfig.set("duration", 10000);
    defaultAppConfig.set("enabled", true);

    Serial.println("⚙️ clock app not defined — auto-creating in /apps.");
    Firebase.RTDB.setJSON(&fbdo, clockAppPath.c_str(), &defaultAppConfig);
  } else {
    Serial.println("📦 clock app already defined in Firebase.");
  }

  bool hasValidSequence = false;
  if (Firebase.RTDB.getArray(&fbdo, appSeqPath.c_str())) {
    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, fbdo.payload().c_str());
    hasValidSequence = !err && doc.is<JsonArray>() && doc.size() > 0;
    if (hasValidSequence) {
      Serial.printf("✅ appSequence is a valid array with %d items.\n", doc.size());
    }
  }

  if (!hasValidSequence) {
    Serial.println("⚠️ appSequence missing or invalid. Writing default...");
    FirebaseJsonArray defaultSeq;
    defaultSeq.add("clock");
    FirebaseJson json;
    json.set("appSequence", defaultSeq);
    Firebase.RTDB.updateNode(&fbdo, settingsPath.c_str(), &json);
  }

  String unitsPath = settingsPath + "/units";
  String timeFmtPath = settingsPath + "/timeFormat";
  String brightnessPath = settingsPath + "/brightness";

  if (Firebase.RTDB.getString(&fbdo, unitsPath.c_str())) {
    units = fbdo.stringData();
    Serial.println("📏 Units loaded: " + units);
  }

  if (Firebase.RTDB.getInt(&fbdo, timeFmtPath.c_str())) {
    int format = fbdo.intData();
    timeFormatPreference = constrain(format, 0, 2);
    Serial.printf("🕒 Time format loaded: %d\n", timeFormatPreference);
  } else {
    timeFormatPreference = 1;
    Firebase.RTDB.setInt(&fbdo, timeFmtPath.c_str(), timeFormatPreference);
    Serial.println("🕒 Default time format set to 12hr (1)");
  }

  int brightness = 7;
  if (Firebase.RTDB.getInt(&fbdo, brightnessPath.c_str())) {
    brightness = fbdo.intData();
    Serial.printf("💡 Brightness loaded: %d\n", brightness);
  } else {
    Firebase.RTDB.setInt(&fbdo, brightnessPath.c_str(), brightness);
    Serial.println("⚠️ Brightness fallback set to 7");
  }
  brightnessLevel = constrain(brightness, 1, 10);

  if (deferGeo) {
    Serial.println("🌐 Skipping GeoIP and Timezone for now — deferGeo = true");
    return;
  }

  updateGeoLocationAndTimezone(settingsPath);
  Serial.println("📍 Settings initialized or updated.");
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