// RemoteConfigManager.h
#pragma once
#include <Firebase_ESP_Client.h>
#include <map>

class RemoteConfigManager {
public:
  static void begin();
  static String get(const String& key, const String& fallback = "");
  static bool has(const String& key);

private:
  static std::map<String, String> configMap;
  static bool fetched;
};