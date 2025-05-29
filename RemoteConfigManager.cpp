#include "RemoteConfigManager.h"
#include <Firebase_ESP_Client.h>
#include "SecretsManager.h"
#include "DeviceRegistration.h"

FirebaseData remoteFbdo;
std::map<String, String> RemoteConfigManager::configMap;
bool RemoteConfigManager::fetched = false;

void RemoteConfigManager::begin() {
  if (!Firebase.ready()) {
    Serial.println("⚠️ Firebase not ready. Cannot fetch remote config.");
    return;
  }

  String globalPath = "/novaFrame/remoteConfig";

  if (!Firebase.RTDB.getJSON(&remoteFbdo, globalPath.c_str())) {
    Serial.printf("❌ Failed to load global remote config: %s\n", remoteFbdo.errorReason().c_str());
    return;
  }

  DynamicJsonDocument doc(2048);
  auto err = deserializeJson(doc, remoteFbdo.payload().c_str());
  if (err) {
    Serial.printf("❌ Failed to parse global remote config: %s\n", err.c_str());
    return;
  }

  for (JsonPair kv : doc.as<JsonObject>()) {
    configMap[kv.key().c_str()] = kv.value().as<String>();
    Serial.printf("🔧 Config key: %s = %s\n", kv.key().c_str(), kv.value().as<String>().c_str());
  }

  fetched = true;
  Serial.println("✅ Remote config loaded.");
}

String RemoteConfigManager::get(const String& key, const String& fallback) {
  if (!fetched) return fallback;
  return configMap.count(key) ? configMap[key] : fallback;
}

bool RemoteConfigManager::has(const String& key) {
  return fetched && configMap.count(key);
}