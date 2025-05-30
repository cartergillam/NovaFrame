#include "FirebaseHelper.h"
#include <Firebase_ESP_Client.h>
#include <vector>
#include <ArduinoJson.h>
#include "DeviceRegistration.h"

extern FirebaseData fbdo;
extern String deviceID;

bool getEnabledAppsFromFirebase(std::vector<String>& enabledApps, bool forceRefresh) {
  static String lastJsonStr = "";
  static std::vector<String> lastEnabledApps;

  if (!forceRefresh && !lastEnabledApps.empty()) {
    enabledApps = lastEnabledApps;
    return true;
  }

  if (!Firebase.ready()) {
    Serial.println("⚠️ Firebase not ready, skipping app fetch");
    return false;
  }

  String appsPath = "/novaFrame/devices/" + deviceID + "/apps";
  if (!Firebase.RTDB.getJSON(&fbdo, appsPath.c_str())) {
    Serial.printf("❌ Failed to get apps JSON: %s\n", fbdo.errorReason().c_str());
    return false;
  }

  String jsonStr;
  fbdo.jsonObject().toString(jsonStr, true);

  // If same as last successful JSON, skip parsing
  if (!forceRefresh && jsonStr == lastJsonStr) {
    enabledApps = lastEnabledApps;
    return true;
  }

  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, jsonStr);
  if (err) {
    Serial.printf("❌ Failed to parse JSON: %s\n", err.c_str());
    return false;
  }

  std::vector<String> apps;
  for (JsonPair kv : doc.as<JsonObject>()) {
    const char* appName = kv.key().c_str();
    JsonObject appData = kv.value().as<JsonObject>();
    if (appData["enabled"] == true) {
      apps.push_back(String(appName));
    }
  }

  enabledApps = apps;
  lastEnabledApps = apps;
  lastJsonStr = jsonStr;
  return true;
}

bool fetchAppSequenceFromFirebase(std::vector<String>& sequence, bool forceRefresh) {
  static String lastJsonStr = "";
  static std::vector<String> lastSequence;

  if (!forceRefresh && !lastSequence.empty()) {
    sequence = lastSequence;
    return true;
  }

  if (!Firebase.ready()) {
    Serial.println("⚠️ Firebase not ready, skipping appSequence fetch");
    return false;
  }

  String path = "/novaFrame/devices/" + deviceID + "/settings/appSequence";
  Serial.println("📡 Fetching appSequence from: " + path);

  if (!Firebase.RTDB.getArray(&fbdo, path.c_str())) {
    Serial.println("❌ Failed to fetch appSequence: " + fbdo.errorReason());
    return false;
  }

  FirebaseJsonArray& arr = fbdo.jsonArray();

  String jsonStr;
  arr.toString(jsonStr, true);

  if (!forceRefresh && jsonStr == lastJsonStr) {
    sequence = lastSequence;
    return true;
  }

  std::vector<String> newSequence;
  for (size_t i = 0; i < arr.size(); i++) {
    FirebaseJsonData val;
    if (arr.get(val, i) && val.type == "string") {
      newSequence.push_back(val.stringValue);
      Serial.println("➡️ App[" + String(i) + "]: " + val.stringValue);
    }
  }

  if (newSequence.empty()) {
    Serial.println("⚠️ Empty or invalid appSequence.");
    return false;
  }

  lastSequence = newSequence;
  lastJsonStr = jsonStr;
  sequence = newSequence;

  Serial.println("✅ appSequence is a valid array with " + String(sequence.size()) + " items.");
  return true;
}

bool setAppSequenceToFirebase(const std::vector<String>& sequence) {
  String path = "/novaFrame/devices/" + deviceID + "/settings";

  FirebaseJsonArray jsonArray;
  for (const auto& app : sequence) {
    jsonArray.add(app);
  }

  FirebaseJson json;
  json.set("appSequence", jsonArray);

  if (!Firebase.RTDB.updateNode(&fbdo, path.c_str(), &json)) {
    Serial.printf("❌ Failed to write appSequence: %s\n", fbdo.errorReason().c_str());
    return false;
  }

  Serial.println("✅ appSequence written to Firebase.");
  return true;
}