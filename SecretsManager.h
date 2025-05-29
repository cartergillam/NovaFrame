// SecretsManager.h
#pragma once
#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>

class SecretsManager {
public:
  static bool load();
  static String get(String key);

private:
  static DynamicJsonDocument doc;
  static bool loaded;
};