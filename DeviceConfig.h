#pragma once

#include <Arduino.h>
#include <map>
#include <vector>

struct TeamFavorite {
  String league = "";
  String teamId = "";
};

struct AppConfig {
  bool enabled = false;
  unsigned long durationMs = 10000;
  String valueMode = "dollar";
  unsigned long symbolDurationMs = 5000;
  std::vector<String> symbols;
  std::vector<TeamFavorite> favorites;
};

struct DaySleepSchedule {
  bool enabled = false;
  String wake = "07:00";
  String sleep = "23:00";
};

struct DeviceSettings {
  std::vector<String> appSequence;
  int brightness = 7;
  int timeFormat = 1;  // 0=12h, 1=12h+AM/PM, 2=24h
  String units = "metric";
  bool autoLocation = true;
  String timezone = "";
  float locationLat = 0.0f;
  float locationLon = 0.0f;
  String locationLabel = "";
  DaySleepSchedule sleepSchedule[7];
};

struct StockRuntimeEntry {
  bool hasQuote = false;
  bool hasTrendline = false;
  double price = 0.0;
  double change = 0.0;
  double changePct = 0.0;
  unsigned long asOf = 0;
  bool stale = false;
  String currency = "USD";
  String quoteState = "pending";
  String trendState = "pending";
  std::vector<float> trendline;
};

struct DeviceStocksRuntime {
  unsigned long generatedAt = 0;
  std::vector<String> symbols;
  std::map<String, StockRuntimeEntry> entries;
  bool available = false;
};

extern DeviceSettings deviceSettings;
extern std::map<String, AppConfig> deviceAppConfigs;
extern DeviceStocksRuntime deviceStocksRuntime;

enum DeviceConfigChangeFlags : uint32_t {
  DeviceConfigChangeNone = 0,
  DeviceConfigChangeBrightness = 1 << 0,
  DeviceConfigChangeTimeFormat = 1 << 1,
  DeviceConfigChangeUnits = 1 << 2,
  DeviceConfigChangeSequence = 1 << 3,
  DeviceConfigChangeAppState = 1 << 4,
  DeviceConfigChangeStocks = 1 << 5,
  DeviceConfigChangeSports = 1 << 6,
  DeviceConfigChangeLocation = 1 << 7,
  DeviceConfigChangeSleepSchedule = 1 << 8,
  DeviceConfigChangeAll = 0xFFFFFFFF
};

bool initializeDeviceConfig(bool allowLocationLookup);
bool refreshDeviceConfig(bool forceRefresh = false);
bool pollDeviceConfigStreams();
uint32_t takeDeviceConfigChangeFlags();
const AppConfig* getAppConfig(const String& appId);
unsigned long getAppDurationMs(const String& appId);
bool isAppEnabled(const String& appId);
bool shouldSleepNow(int weekdayIndex, int minutesSinceMidnight);
const DeviceStocksRuntime& getDeviceStocksRuntime();
bool getStockRuntimeEntry(const String& symbol, StockRuntimeEntry& outEntry);
void clearDeviceStocksRuntime();
void writeDeviceStatus(const String& activeAppId, bool isSleeping, unsigned long unixTime, bool timeSynchronized);
bool maybeAutoUpdateLocationFromGeo(bool force = false);
