#include "FirebaseHelper.h"
#include <Firebase_ESP_Client.h>
#include <vector>
#include <ArduinoJson.h>
#include "DeviceRegistration.h"

extern FirebaseData fbdo;
extern String deviceID;

bool getEnabledAppsFromFirebase(std::vector<String>& enabledApps) {
  if (!Firebase.ready()) {
    Serial.println("⚠️ Firebase not ready, skipping appSequence fetch");
    return false;
  }
  String appsPath = "/novaFrame/devices/" + deviceID + "/apps";

  if (!Firebase.RTDB.getJSON(&fbdo, appsPath.c_str())) {
    Serial.printf("❌ Failed to get apps JSON: %s\n", fbdo.errorReason().c_str());
    return false;
  }

  String jsonStr;
  fbdo.jsonObject().toString(jsonStr, true);

  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, jsonStr);
  if (err) {
    Serial.printf("❌ Failed to parse JSON: %s\n", err.c_str());
    return false;
  }

  for (JsonPair kv : doc.as<JsonObject>()) {
    const char* appName = kv.key().c_str();
    JsonObject appData = kv.value().as<JsonObject>();
    if (appData["enabled"] == true) {
      enabledApps.push_back(String(appName));
    }
  }

  return true;
}

bool fetchAppSequenceFromFirebase(std::vector<String>& sequence) {
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
  size_t len = arr.size();

  Serial.println("✅ appSequence is a valid array with " + String(len) + " items.");

  for (size_t i = 0; i < len; i++) {
    FirebaseJsonData val;
    if (arr.get(val, i) && val.type == "string") {
      sequence.push_back(val.stringValue);
      Serial.println("➡️ App[" + String(i) + "]: " + val.stringValue);
    }
  }

  return !sequence.empty();
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