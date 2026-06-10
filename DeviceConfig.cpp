#include "DeviceConfig.h"

#include <ArduinoJson.h>
#include <Firebase_ESP_Client.h>
#include <HTTPClient.h>
#include <algorithm>
#include <math.h>
#include <map>
#include <stdlib.h>
#include <string.h>

#include "DeviceRegistration.h"
#include "RemoteConfigManager.h"
#include "SecretsManager.h"

extern FirebaseData fbdo;
extern FirebaseAuth auth;
extern FirebaseConfig config;
extern String deviceID;
extern String units;
extern int timeFormatPreference;
extern float storedLat;
extern float storedLon;

DeviceSettings deviceSettings;
std::map<String, AppConfig> deviceAppConfigs;
DeviceStocksRuntime deviceStocksRuntime;

namespace {

FirebaseData configFbdo;
FirebaseData deviceStreamFbdo;
FirebaseData statusFbdo;

constexpr int kSchemaVersion = 5;
constexpr size_t kDayCount = 7;
constexpr unsigned long kConfigStreamRetryMs = 5000;
constexpr unsigned long kAutoLocationIntervalMs = 6UL * 60UL * 60UL * 1000UL;

bool deviceStreamStarted = false;
unsigned long lastDeviceStreamRetryAt = 0;
uint8_t consecutiveStreamReadFailures = 0;
uint32_t pendingConfigChangeFlags = DeviceConfigChangeAll;
unsigned long lastAutoLocationSyncAt = 0;

struct FirebaseTelemetry {
  uint32_t streamBeginAttempts = 0;
  uint32_t streamBeginSuccess = 0;
  uint32_t streamReadCalls = 0;
  uint32_t streamReadFalse = 0;
  uint32_t streamTimeouts = 0;
  uint32_t streamEvents = 0;
  uint32_t streamDrops = 0;
  uint32_t runtimeSnapshotRefreshCalls = 0;
  uint32_t runtimeSnapshotRefreshFailures = 0;
  uint32_t runtimeSnapshotApplies = 0;
  uint32_t configRefreshCalls = 0;
  uint32_t configRefreshFailures = 0;
  uint32_t statusWrites = 0;
};

FirebaseTelemetry firebaseTelemetry;

const char* kDayKeys[kDayCount] = {
  "sun", "mon", "tue", "wed", "thu", "fri", "sat"
};

struct AppSeed {
  const char* id;
  const char* name;
  const char* icon;
  const char* description;
  unsigned long defaultDurationMs;
  bool enabledByDefault;
};

const AppSeed kCanonicalApps[] = {
  {"clockWeather", "Clock + Weather", "clock-cloud", "Primary clock with current weather", 20000, true},
  {"forecast", "2-Day Forecast", "cloud-sun", "Highs/lows with icons and location", 20000, true},
  {"stocks", "Stocks", "chart-line", "Favorite stocks with quotes and trendlines", 20000, true},
  {"mlb", "MLB Scores", "baseball", "Favorite MLB teams and live scores", 20000, false},
  {"nba", "NBA Scores", "basketball", "Favorite NBA teams and live scores", 20000, false},
  {"nfl", "NFL Scores", "football", "Favorite NFL teams and live scores", 20000, false},
  {"nhl", "NHL Scores", "hockey", "Favorite NHL teams and live scores", 20000, false},
};

bool readJsonAtPath(FirebaseData& data, const String& path, DynamicJsonDocument& doc);

String deviceRoot() {
  return "/novaFrame/devices/" + deviceID;
}

String getCatalogPath(const String& appId) {
  return "/novaFrame/apps/" + appId;
}

String getDeviceAppPath(const String& appId) {
  return deviceRoot() + "/apps/" + appId;
}

String getSettingsPath() {
  return deviceRoot() + "/settings";
}

String getStatusPath() {
  return deviceRoot() + "/status";
}

String normalizeAppId(const String& appId) {
  if (appId == "stock") return "stocks";
  return appId;
}

bool isSportsAppId(const String& appId) {
  return appId == "mlb" || appId == "nba" || appId == "nfl" || appId == "nhl" || appId == "sports";
}

String normalizeSymbol(const String& rawSymbol) {
  String symbol = rawSymbol;
  symbol.trim();
  symbol.toUpperCase();
  return symbol;
}

String inferCurrencyForSymbol(const String& rawSymbol) {
  String symbol = normalizeSymbol(rawSymbol);
  if (symbol.endsWith(".TO") ||
      symbol.endsWith(".V") ||
      symbol.endsWith(".NE") ||
      symbol.endsWith(".CNQ") ||
      symbol.endsWith(":CA")) {
    return "CAD";
  }
  return "USD";
}

bool tryParseDoubleValue(JsonVariantConst value, double& output) {
  if (value.is<double>() || value.is<float>()) {
    output = value.as<double>();
    return true;
  }
  if (value.is<long>() || value.is<int>() || value.is<unsigned long>() || value.is<unsigned int>()) {
    output = static_cast<double>(value.as<long>());
    return true;
  }
  const char* raw = value.as<const char*>();
  if (raw == nullptr || strlen(raw) == 0) return false;
  char* endPtr = nullptr;
  double parsed = strtod(raw, &endPtr);
  if (endPtr == raw) return false;
  output = parsed;
  return true;
}

bool tryParseUnsignedLongValue(JsonVariantConst value, unsigned long& output) {
  if (value.is<unsigned long>() || value.is<unsigned int>()) {
    output = value.as<unsigned long>();
    return true;
  }
  if (value.is<long>() || value.is<int>()) {
    long parsed = value.as<long>();
    if (parsed < 0) return false;
    output = static_cast<unsigned long>(parsed);
    return true;
  }
  const char* raw = value.as<const char*>();
  if (raw == nullptr || strlen(raw) == 0) return false;
  char* endPtr = nullptr;
  unsigned long parsed = strtoul(raw, &endPtr, 10);
  if (endPtr == raw) return false;
  output = parsed;
  return true;
}

String urlEncode(const String& input) {
  const char* hex = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(input.length() * 3);
  for (size_t i = 0; i < input.length(); ++i) {
    unsigned char c = static_cast<unsigned char>(input[i]);
    if ((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0x0F];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

bool fetchGeoLocation(float& lat, float& lon, String& label, String& timezone) {
  // Prefer paid provider for better location accuracy on mobile/home ISPs.
  String geoKey = RemoteConfigManager::get("IP_GEO_LOCATION_API_KEY", "");
  if (geoKey.length() > 0) {
    HTTPClient geoHttp;
    geoHttp.begin("https://api.ipgeolocation.io/ipgeo?apiKey=" + geoKey);
    int geoCode = geoHttp.GET();
    if (geoCode == 200) {
      DynamicJsonDocument geoDoc(4096);
      DeserializationError geoErr = deserializeJson(geoDoc, geoHttp.getString());
      geoHttp.end();
      if (!geoErr) {
        String latRaw = String(geoDoc["latitude"] | "");
        String lonRaw = String(geoDoc["longitude"] | "");
        lat = latRaw.toFloat();
        lon = lonRaw.toFloat();

        String city = String(geoDoc["city"] | "");
        String region = String(geoDoc["state_prov"] | "");
        timezone = String(geoDoc["time_zone"]["name"] | "");
        String ip = String(geoDoc["ip"] | "");
        String isp = String(geoDoc["isp"] | "");

        label = city;
        if (region.length() > 0) {
          if (label.length() > 0) label += ", ";
          label += region;
        }

        if (lat != 0.0f && lon != 0.0f) {
          Serial.println("📍 GeoIP source: ipgeolocation.io");
          Serial.println("   ip=" + ip + " isp=" + isp);
          Serial.println("   label=" + label + " lat=" + String(lat, 6) + " lon=" + String(lon, 6));
          return true;
        }
      } else {
        Serial.println("⚠️ Failed to parse ipgeolocation.io response, using fallback.");
      }
    } else {
      Serial.println("⚠️ ipgeolocation.io lookup failed: " + geoHttp.errorToString(geoCode));
      geoHttp.end();
    }
  }

  // Fallback provider.
  HTTPClient fallbackHttp;
  fallbackHttp.begin("http://ip-api.com/json");
  int fallbackCode = fallbackHttp.GET();
  if (fallbackCode != 200) {
    Serial.println("❌ GeoIP bootstrap failed: " + fallbackHttp.errorToString(fallbackCode));
    fallbackHttp.end();
    return false;
  }

  DynamicJsonDocument fallbackDoc(2048);
  DeserializationError fallbackErr = deserializeJson(fallbackDoc, fallbackHttp.getString());
  fallbackHttp.end();
  if (fallbackErr) {
    Serial.println("❌ Failed to parse fallback GeoIP response.");
    return false;
  }

  lat = fallbackDoc["lat"] | 0.0f;
  lon = fallbackDoc["lon"] | 0.0f;
  String city = fallbackDoc["city"] | "";
  String region = fallbackDoc["region"] | "";
  timezone = fallbackDoc["timezone"] | "";
  String queryIp = fallbackDoc["query"] | "";
  String isp = fallbackDoc["isp"] | "";
  label = city;
  if (region.length() > 0) {
    if (label.length() > 0) label += ", ";
    label += region;
  }

  Serial.println("📍 GeoIP source: ip-api.com (fallback)");
  Serial.println("   ip=" + queryIp + " isp=" + isp);
  Serial.println("   label=" + label + " lat=" + String(lat, 6) + " lon=" + String(lon, 6));

  return lat != 0.0f && lon != 0.0f;
}

bool stringVectorsEqual(const std::vector<String>& left, const std::vector<String>& right) {
  if (left.size() != right.size()) return false;
  for (size_t i = 0; i < left.size(); ++i) {
    if (left[i] != right[i]) return false;
  }
  return true;
}

bool favoritesEqual(const std::vector<TeamFavorite>& left, const std::vector<TeamFavorite>& right) {
  if (left.size() != right.size()) return false;
  for (size_t i = 0; i < left.size(); ++i) {
    if (left[i].league != right[i].league || left[i].teamId != right[i].teamId) {
      return false;
    }
  }
  return true;
}

bool dayScheduleEqual(const DaySleepSchedule& left, const DaySleepSchedule& right) {
  return left.enabled == right.enabled &&
         left.wake == right.wake &&
         left.sleep == right.sleep;
}

bool stockEntryEqual(const StockRuntimeEntry& left, const StockRuntimeEntry& right) {
  if (left.hasQuote != right.hasQuote ||
      left.hasTrendline != right.hasTrendline ||
      left.price != right.price ||
      left.change != right.change ||
      left.changePct != right.changePct ||
      left.asOf != right.asOf ||
      left.stale != right.stale ||
      left.quoteState != right.quoteState ||
      left.trendState != right.trendState ||
      left.trendline.size() != right.trendline.size()) {
    return false;
  }

  for (size_t i = 0; i < left.trendline.size(); ++i) {
    if (left.trendline[i] != right.trendline[i]) {
      return false;
    }
  }
  return true;
}

bool stockRuntimeEqual(const DeviceStocksRuntime& left, const DeviceStocksRuntime& right) {
  if (left.available != right.available ||
      left.generatedAt != right.generatedAt ||
      !stringVectorsEqual(left.symbols, right.symbols) ||
      left.entries.size() != right.entries.size()) {
    return false;
  }

  for (const auto& entry : left.entries) {
    auto it = right.entries.find(entry.first);
    if (it == right.entries.end()) return false;
    if (!stockEntryEqual(entry.second, it->second)) return false;
  }
  return true;
}

void appendSymbolIfMissing(std::vector<String>& symbols, const String& rawSymbol) {
  String symbol = normalizeSymbol(rawSymbol);
  if (symbol.length() == 0) return;
  if (std::find(symbols.begin(), symbols.end(), symbol) == symbols.end()) {
    symbols.push_back(symbol);
  }
}

bool parseStreamPayload(FirebaseData& streamFbdo, DynamicJsonDocument& doc) {
  String payload = streamFbdo.jsonString();
  if (payload.length() == 0) {
    String type = streamFbdo.dataType();
    if (type == "json") {
      streamFbdo.jsonObject().toString(payload, true);
    } else if (type == "array") {
      streamFbdo.jsonArray().toString(payload, true);
    } else {
      payload = streamFbdo.stringData();
    }
  }

  if (payload.length() == 0) {
    return false;
  }

  return !deserializeJson(doc, payload);
}

bool parseStreamIntValue(FirebaseData& streamFbdo, int& outValue) {
  String type = streamFbdo.dataType();
  type.toLowerCase();

  if (type == "int") {
    outValue = streamFbdo.intData();
    return true;
  }

  String payload = streamFbdo.stringData();
  payload.trim();
  if (payload.length() == 0) {
    return false;
  }

  char* endPtr = nullptr;
  long parsed = strtol(payload.c_str(), &endPtr, 10);
  if (endPtr == payload.c_str()) {
    return false;
  }

  outValue = static_cast<int>(parsed);
  return true;
}

bool parseStreamUnsignedLongValue(FirebaseData& streamFbdo, unsigned long& outValue) {
  String type = streamFbdo.dataType();
  type.toLowerCase();

  if (type == "int") {
    int parsed = streamFbdo.intData();
    if (parsed < 0) return false;
    outValue = static_cast<unsigned long>(parsed);
    return true;
  }

  String payload = streamFbdo.stringData();
  payload.trim();
  if (payload.length() == 0) {
    return false;
  }

  char* endPtr = nullptr;
  unsigned long parsed = strtoul(payload.c_str(), &endPtr, 10);
  if (endPtr == payload.c_str()) {
    return false;
  }

  outValue = parsed;
  return true;
}

bool parseStreamBoolValue(FirebaseData& streamFbdo, bool& outValue) {
  String type = streamFbdo.dataType();
  type.toLowerCase();

  if (type == "boolean") {
    outValue = streamFbdo.boolData();
    return true;
  }
  if (type == "int") {
    outValue = streamFbdo.intData() != 0;
    return true;
  }

  String payload = streamFbdo.stringData();
  payload.trim();
  payload.toLowerCase();
  if (payload == "true" || payload == "1") {
    outValue = true;
    return true;
  }
  if (payload == "false" || payload == "0") {
    outValue = false;
    return true;
  }
  return false;
}

bool parseStreamFloatValue(FirebaseData& streamFbdo, float& outValue) {
  String type = streamFbdo.dataType();
  type.toLowerCase();

  if (type == "float" || type == "double") {
    outValue = static_cast<float>(streamFbdo.floatData());
    return true;
  }
  if (type == "int") {
    outValue = static_cast<float>(streamFbdo.intData());
    return true;
  }

  String payload = streamFbdo.stringData();
  payload.trim();
  if (payload.length() == 0) {
    return false;
  }

  char* endPtr = nullptr;
  float parsed = strtof(payload.c_str(), &endPtr);
  if (endPtr == payload.c_str()) {
    return false;
  }
  outValue = parsed;
  return true;
}

bool parseStreamStringValue(FirebaseData& streamFbdo, String& outValue) {
  String type = streamFbdo.dataType();
  type.toLowerCase();
  if (type == "string") {
    outValue = streamFbdo.stringData();
    return true;
  }

  DynamicJsonDocument doc(256);
  if (parseStreamPayload(streamFbdo, doc)) {
    if (doc.is<const char*>()) {
      outValue = String(doc.as<const char*>());
      return true;
    }
    if (doc.is<String>()) {
      outValue = doc.as<String>();
      return true;
    }
  }
  return false;
}

bool hasActionableStreamError(const String& reason) {
  for (size_t i = 0; i < reason.length(); ++i) {
    char c = reason[i];
    if ((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) {
      return true;
    }
  }
  return false;
}

bool applyStockRuntimeJson(JsonObjectConst stocksObject) {
  if (stocksObject.isNull()) {
    if (deviceStocksRuntime.available || !deviceStocksRuntime.entries.empty()) {
      deviceStocksRuntime = DeviceStocksRuntime();
      pendingConfigChangeFlags |= DeviceConfigChangeStocks;
    }
    return true;
  }

  DeviceStocksRuntime nextRuntime;
  nextRuntime.available = true;
  tryParseUnsignedLongValue(stocksObject["generatedAt"], nextRuntime.generatedAt);

  JsonArrayConst symbols = stocksObject["symbols"].as<JsonArrayConst>();
  if (!symbols.isNull()) {
    for (JsonVariantConst symbolValue : symbols) {
      const char* rawSymbol = symbolValue.as<const char*>();
      appendSymbolIfMissing(nextRuntime.symbols, rawSymbol == nullptr ? "" : String(rawSymbol));
    }
  }

  JsonObjectConst quotes = stocksObject["quotes"].as<JsonObjectConst>();
  JsonObjectConst trendlines = stocksObject["trendlines"].as<JsonObjectConst>();
  JsonObjectConst state = stocksObject["state"].as<JsonObjectConst>();

  if (!quotes.isNull()) {
    for (JsonPairConst kv : quotes) {
      appendSymbolIfMissing(nextRuntime.symbols, String(kv.key().c_str()));
    }
  }
  if (!trendlines.isNull()) {
    for (JsonPairConst kv : trendlines) {
      appendSymbolIfMissing(nextRuntime.symbols, String(kv.key().c_str()));
    }
  }
  if (!state.isNull()) {
    for (JsonPairConst kv : state) {
      appendSymbolIfMissing(nextRuntime.symbols, String(kv.key().c_str()));
    }
  }

  for (const String& symbol : nextRuntime.symbols) {
    StockRuntimeEntry entry;

    JsonObjectConst quote = quotes[symbol.c_str()].as<JsonObjectConst>();
    if (!quote.isNull()) {
      double parsed = 0.0;
      if (tryParseDoubleValue(quote["price"], parsed)) {
        entry.price = parsed;
        entry.hasQuote = true;
      }
      if (tryParseDoubleValue(quote["change"], parsed)) {
        entry.change = parsed;
      }
      if (tryParseDoubleValue(quote["changePct"], parsed)) {
        entry.changePct = parsed;
      }
      unsigned long asOf = 0;
      if (tryParseUnsignedLongValue(quote["asOf"], asOf)) {
        entry.asOf = asOf;
      }
      entry.stale = quote["stale"] | false;
      String currency = String(quote["currency"] | "");
      currency.trim();
      currency.toUpperCase();
      entry.currency = currency.length() == 0 ? inferCurrencyForSymbol(symbol) : currency;
    } else {
      entry.currency = inferCurrencyForSymbol(symbol);
    }

    JsonArrayConst trendline = trendlines[symbol.c_str()].as<JsonArrayConst>();
    if (!trendline.isNull()) {
      for (JsonVariantConst value : trendline) {
        entry.trendline.push_back(static_cast<float>(value.as<double>()));
      }
      entry.hasTrendline = !entry.trendline.empty();
    }

    JsonObjectConst symbolState = state[symbol.c_str()].as<JsonObjectConst>();
    if (!symbolState.isNull()) {
      entry.quoteState = String(symbolState["quoteState"] | (entry.hasQuote ? "ok" : "pending"));
      entry.trendState = String(symbolState["trendState"] | (entry.hasTrendline ? "ok" : "pending"));
    } else {
      entry.quoteState = entry.hasQuote ? "ok" : "pending";
      entry.trendState = entry.hasTrendline ? "ok" : "pending";
    }

    nextRuntime.entries[symbol] = entry;
  }

  bool changed = !stockRuntimeEqual(deviceStocksRuntime, nextRuntime);
  deviceStocksRuntime = nextRuntime;
  firebaseTelemetry.runtimeSnapshotApplies++;
  if (changed) {
    pendingConfigChangeFlags |= DeviceConfigChangeStocks;
  }
  return true;
}

bool refreshDeviceStocksRuntimeSnapshot(bool logFailure) {
  firebaseTelemetry.runtimeSnapshotRefreshCalls++;
  DynamicJsonDocument doc(24576);
  if (!readJsonAtPath(configFbdo, deviceRoot() + "/runtime/stocks", doc)) {
    firebaseTelemetry.runtimeSnapshotRefreshFailures++;
    if (logFailure) {
      Serial.println("⚠️ Failed to load runtime stocks snapshot: " + configFbdo.errorReason());
    }
    return false;
  }
  JsonObjectConst stocksRoot = doc.as<JsonObjectConst>();
  return applyStockRuntimeJson(stocksRoot);
}

bool readJsonAtPath(FirebaseData& data, const String& path, DynamicJsonDocument& doc) {
  if (!Firebase.RTDB.getJSON(&data, path.c_str())) {
    return false;
  }

  String jsonStr;
  data.jsonObject().toString(jsonStr, true);
  return !deserializeJson(doc, jsonStr);
}

void ensureCatalogEntry(const AppSeed& app) {
  String path = getCatalogPath(app.id);
  bool exists = Firebase.RTDB.getJSON(&configFbdo, path.c_str());
  bool needsWrite = !exists;
  bool removeLegacyDuration = false;
  FirebaseJson json;

  if (exists) {
    String jsonStr;
    configFbdo.jsonObject().toString(jsonStr, true);
    StaticJsonDocument<1024> doc;
    if (!deserializeJson(doc, jsonStr)) {
      if (String(doc["name"] | "") != String(app.name)) {
        json.set("name", app.name);
        needsWrite = true;
      }
      if (String(doc["icon"] | "") != String(app.icon)) {
        json.set("icon", app.icon);
        needsWrite = true;
      }
      if (String(doc["description"] | "") != String(app.description)) {
        json.set("description", app.description);
        needsWrite = true;
      }
      if ((doc["defaultDurationMs"] | 0UL) != app.defaultDurationMs) {
        json.set("defaultDurationMs", static_cast<int>(app.defaultDurationMs));
        needsWrite = true;
      }
      if ((doc["enabledByDefault"] | false) != app.enabledByDefault) {
        json.set("enabledByDefault", app.enabledByDefault);
        needsWrite = true;
      }
      removeLegacyDuration = doc.containsKey("defaultDuration");
    } else {
      json.set("name", app.name);
      json.set("icon", app.icon);
      json.set("description", app.description);
      json.set("defaultDurationMs", static_cast<int>(app.defaultDurationMs));
      json.set("enabledByDefault", app.enabledByDefault);
      needsWrite = true;
    }
  }

  if (!exists || needsWrite) {
    if (!exists) {
      json.set("name", app.name);
      json.set("icon", app.icon);
      json.set("description", app.description);
      json.set("defaultDurationMs", static_cast<int>(app.defaultDurationMs));
      json.set("enabledByDefault", app.enabledByDefault);
      Firebase.RTDB.setJSON(&configFbdo, path.c_str(), &json);
    } else {
      Firebase.RTDB.updateNode(&configFbdo, path.c_str(), &json);
    }
  }

  if (removeLegacyDuration) {
    Firebase.RTDB.deleteNode(&configFbdo, (path + "/defaultDuration").c_str());
  }
}

void ensureCatalogSchema() {
  for (const auto& app : kCanonicalApps) {
    ensureCatalogEntry(app);
  }
  Firebase.RTDB.deleteNode(&configFbdo, "/novaFrame/apps/stock");
}

void cleanupCatalogDurations() {
  if (!Firebase.RTDB.getJSON(&configFbdo, "/novaFrame/apps")) {
    return;
  }

  String jsonStr;
  configFbdo.jsonObject().toString(jsonStr, true);
  StaticJsonDocument<4096> doc;
  if (deserializeJson(doc, jsonStr)) {
    return;
  }

  for (JsonPair kv : doc.as<JsonObject>()) {
    String appId = String(kv.key().c_str());
    JsonObject app = kv.value().as<JsonObject>();
    if (!app.containsKey("defaultDurationMs") && app.containsKey("defaultDuration")) {
      Firebase.RTDB.setInt(&configFbdo, (getCatalogPath(appId) + "/defaultDurationMs").c_str(), app["defaultDuration"].as<int>());
    }
    if (app.containsKey("defaultDuration")) {
      Firebase.RTDB.deleteNode(&configFbdo, (getCatalogPath(appId) + "/defaultDuration").c_str());
    }
  }
}

void setDefaultSymbols(FirebaseJson& json) {
  FirebaseJsonArray symbols;
  symbols.add("AAPL");
  symbols.add("MSFT");
  symbols.add("TSLA");
  json.set("symbols", symbols);
}

void setDefaultFavorites(FirebaseJson& json) {
  FirebaseJsonArray favorites;
  json.set("favorites", favorites);
}

void seedAppIfMissing(const AppSeed& seed) {
  if (Firebase.RTDB.getJSON(&configFbdo, getDeviceAppPath(seed.id).c_str())) {
    return;
  }

  FirebaseJson json;
  json.set("enabled", seed.enabledByDefault);
  json.set("durationMs", static_cast<int>(seed.defaultDurationMs));
  if (String(seed.id) == "stocks") {
    json.set("valueMode", "dollar");
    json.set("symbolDurationMs", 5000);
    setDefaultSymbols(json);
  } else if (isSportsAppId(String(seed.id))) {
    setDefaultFavorites(json);
  }
  Firebase.RTDB.setJSON(&configFbdo, getDeviceAppPath(seed.id).c_str(), &json);
}

void seedDefaultSettings(bool settingsExists) {
  if (settingsExists) return;

  FirebaseJson json;
  FirebaseJsonArray seq;
  seq.add("clockWeather");
  seq.add("forecast");
  seq.add("stocks");
  json.set("appSequence", seq);
  json.set("brightness", 7);
  json.set("timeFormat", 1);
  json.set("units", "metric");
  json.set("autoLocation", true);

  json.set("location/lat", 0.0f);
  json.set("location/lon", 0.0f);
  json.set("location/label", "");
  json.set("timezone", "");

  for (size_t i = 0; i < kDayCount; ++i) {
    String dayPath = "sleepSchedule/";
    dayPath += kDayKeys[i];
    json.set(dayPath + "/enabled", false);
    json.set(dayPath + "/wake", "07:00");
    json.set(dayPath + "/sleep", "23:00");
  }

  Firebase.RTDB.updateNode(&configFbdo, getSettingsPath().c_str(), &json);
}

void migrateCoreSettings() {
  if (!Firebase.RTDB.getJSON(&configFbdo, getSettingsPath().c_str())) {
    return;
  }

  String jsonStr;
  configFbdo.jsonObject().toString(jsonStr, true);
  StaticJsonDocument<4096> doc;
  if (deserializeJson(doc, jsonStr)) {
    return;
  }

  FirebaseJson json;
  bool needsUpdate = false;

  if (!doc.containsKey("brightness")) {
    json.set("brightness", 7);
    needsUpdate = true;
  }
  if (!doc.containsKey("timeFormat")) {
    json.set("timeFormat", 1);
    needsUpdate = true;
  }
  if (!doc.containsKey("units")) {
    json.set("units", "metric");
    needsUpdate = true;
  }
  if (!doc.containsKey("autoLocation")) {
    json.set("autoLocation", true);
    needsUpdate = true;
  }
  if (!doc.containsKey("timezone")) {
    json.set("timezone", "");
    needsUpdate = true;
  }

  JsonArray sequence = doc["appSequence"].as<JsonArray>();
  if (sequence.isNull() || sequence.size() == 0) {
    FirebaseJsonArray defaultSeq;
    defaultSeq.add("clockWeather");
    defaultSeq.add("forecast");
    defaultSeq.add("stocks");
    json.set("appSequence", defaultSeq);
    needsUpdate = true;
  } else {
    FirebaseJsonArray migratedSeq;
    bool sequenceChanged = false;
    for (JsonVariant v : sequence) {
      const char* rawId = v.as<const char*>();
      String originalId = rawId == nullptr ? "" : String(rawId);
      String appId = normalizeAppId(originalId);
      if (appId.length() == 0) continue;
      if (appId != originalId) {
        sequenceChanged = true;
      }
      migratedSeq.add(appId);
    }
    if (sequenceChanged) {
      json.set("appSequence", migratedSeq);
      needsUpdate = true;
    }
  }

  if (needsUpdate) {
    Firebase.RTDB.updateNode(&configFbdo, getSettingsPath().c_str(), &json);
  }
}

void migrateLegacyStockConfig() {
  String legacyPath = getDeviceAppPath("stock");
  if (!Firebase.RTDB.getJSON(&configFbdo, legacyPath.c_str())) {
    return;
  }

  String jsonStr;
  configFbdo.jsonObject().toString(jsonStr, true);
  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, jsonStr)) {
    return;
  }

  FirebaseJson migrated;
  bool enabled = doc["enabled"] | true;
  unsigned long durationMs = doc["durationMs"] | 0UL;
  if (durationMs == 0UL) {
    durationMs = doc["duration"] | 20000UL;
  }
  migrated.set("enabled", enabled);
  migrated.set("durationMs", static_cast<int>(durationMs));
  migrated.set("valueMode", "dollar");
  migrated.set("symbolDurationMs", 5000);

  FirebaseJsonArray symbols;
  JsonArray oldSymbols = doc["symbols"]["symbols"].as<JsonArray>();
  if (!oldSymbols.isNull()) {
    for (JsonVariant v : oldSymbols) {
      const char* rawSymbol = v.as<const char*>();
      if (rawSymbol != nullptr) symbols.add(rawSymbol);
    }
  }
  migrated.set("symbols", symbols);

  Firebase.RTDB.updateNode(&configFbdo, getDeviceAppPath("stocks").c_str(), &migrated);
  Firebase.RTDB.deleteNode(&configFbdo, legacyPath.c_str());
}

bool hasFavorite(const std::vector<TeamFavorite>& favorites, const TeamFavorite& candidate) {
  for (const TeamFavorite& favorite : favorites) {
    if (favorite.league == candidate.league && favorite.teamId == candidate.teamId) {
      return true;
    }
  }
  return false;
}

void writeFavoritesArray(const String& appId, const std::vector<TeamFavorite>& favorites) {
  Firebase.RTDB.deleteNode(&configFbdo, (getDeviceAppPath(appId) + "/favorites").c_str());
  FirebaseJson json;
  for (size_t i = 0; i < favorites.size(); ++i) {
    String base = "favorites/[";
    base += static_cast<int>(i);
    base += "]";
    json.set(base + "/league", favorites[i].league);
    json.set(base + "/teamId", favorites[i].teamId);
  }
  Firebase.RTDB.updateNode(&configFbdo, getDeviceAppPath(appId).c_str(), &json);
}

std::vector<TeamFavorite> readFavoritesFromAppDoc(JsonObject app, const String& fallbackLeague) {
  std::vector<TeamFavorite> favorites;
  JsonArray rawFavorites = app["favorites"].as<JsonArray>();
  if (rawFavorites.isNull()) return favorites;

  for (JsonVariant v : rawFavorites) {
    TeamFavorite favorite;
    favorite.league = String(v["league"] | fallbackLeague);
    favorite.league.toLowerCase();
    favorite.teamId = String(v["teamId"] | "");
    favorite.teamId.toUpperCase();
    if (favorite.league.length() == 0) favorite.league = fallbackLeague;
    if (favorite.teamId.length() > 0 && !hasFavorite(favorites, favorite)) {
      favorites.push_back(favorite);
    }
  }
  return favorites;
}

void updateSequenceForMigratedSports(const std::vector<String>& enabledLeagues) {
  DynamicJsonDocument settingsDoc(4096);
  if (!readJsonAtPath(configFbdo, getSettingsPath(), settingsDoc)) {
    return;
  }

  std::vector<String> nextSequence;
  JsonArray sequence = settingsDoc["appSequence"].as<JsonArray>();
  if (!sequence.isNull()) {
    for (JsonVariant v : sequence) {
      const char* rawId = v.as<const char*>();
      String appId = normalizeAppId(rawId == nullptr ? "" : String(rawId));
      if (appId == "sports" || appId.length() == 0) continue;
      if (std::find(nextSequence.begin(), nextSequence.end(), appId) == nextSequence.end()) {
        nextSequence.push_back(appId);
      }
    }
  }

  for (const String& league : enabledLeagues) {
    if (std::find(nextSequence.begin(), nextSequence.end(), league) == nextSequence.end()) {
      nextSequence.push_back(league);
    }
  }

  FirebaseJsonArray seq;
  for (const String& appId : nextSequence) {
    seq.add(appId);
  }
  Firebase.RTDB.setArray(&configFbdo, (getSettingsPath() + "/appSequence").c_str(), &seq);
}

void migrateLegacySportsConfig() {
  String legacyPath = getDeviceAppPath("sports");
  if (!Firebase.RTDB.getJSON(&configFbdo, legacyPath.c_str())) {
    return;
  }

  String jsonStr;
  configFbdo.jsonObject().toString(jsonStr, true);
  StaticJsonDocument<4096> legacyDoc;
  if (deserializeJson(legacyDoc, jsonStr)) {
    return;
  }

  JsonObject legacyApp = legacyDoc.as<JsonObject>();
  std::vector<TeamFavorite> legacyFavorites = readFavoritesFromAppDoc(legacyApp, "");
  if (legacyFavorites.empty()) {
    std::vector<String> emptyLeagues;
    updateSequenceForMigratedSports(emptyLeagues);
    Firebase.RTDB.deleteNode(&configFbdo, legacyPath.c_str());
    return;
  }

  std::vector<String> enabledLeagues;
  const char* leagues[] = {"mlb", "nba", "nfl", "nhl"};
  for (const char* rawLeague : leagues) {
    String league(rawLeague);
    std::vector<TeamFavorite> merged;

    DynamicJsonDocument existingDoc(2048);
    if (readJsonAtPath(configFbdo, getDeviceAppPath(league), existingDoc)) {
      merged = readFavoritesFromAppDoc(existingDoc.as<JsonObject>(), league);
    }

    bool changed = false;
    for (const TeamFavorite& favorite : legacyFavorites) {
      if (favorite.league != league || hasFavorite(merged, favorite)) continue;
      merged.push_back(favorite);
      changed = true;
    }

    if (changed) {
      writeFavoritesArray(league, merged);
      Firebase.RTDB.setBool(&configFbdo, (getDeviceAppPath(league) + "/enabled").c_str(), true);
      enabledLeagues.push_back(league);
    }
  }

  updateSequenceForMigratedSports(enabledLeagues);
  Firebase.RTDB.deleteNode(&configFbdo, legacyPath.c_str());
}

void migratePerAppDurations() {
  if (!Firebase.RTDB.getJSON(&configFbdo, (deviceRoot() + "/apps").c_str())) {
    return;
  }

  String jsonStr;
  configFbdo.jsonObject().toString(jsonStr, true);
  StaticJsonDocument<4096> doc;
  if (deserializeJson(doc, jsonStr)) {
    return;
  }

  for (JsonPair kv : doc.as<JsonObject>()) {
    String appId = normalizeAppId(String(kv.key().c_str()));
    JsonObject app = kv.value().as<JsonObject>();
    if (!app.containsKey("durationMs") && app.containsKey("duration")) {
      Firebase.RTDB.setInt(&configFbdo, (getDeviceAppPath(appId) + "/durationMs").c_str(), app["duration"].as<int>());
    }
    if (app.containsKey("duration")) {
      Firebase.RTDB.deleteNode(&configFbdo, (getDeviceAppPath(appId) + "/duration").c_str());
    }
    if (appId == "stocks" && app["symbols"].is<JsonObject>() && app["symbols"]["symbols"].is<JsonArray>()) {
      FirebaseJsonArray symbols;
      for (JsonVariant v : app["symbols"]["symbols"].as<JsonArray>()) {
        const char* rawSymbol = v.as<const char*>();
        if (rawSymbol != nullptr) symbols.add(rawSymbol);
      }
      Firebase.RTDB.setArray(&configFbdo, (getDeviceAppPath(appId) + "/symbols").c_str(), &symbols);
    }
    if (appId == "stocks" && !app.containsKey("symbolDurationMs")) {
      Firebase.RTDB.setInt(&configFbdo, (getDeviceAppPath(appId) + "/symbolDurationMs").c_str(), 5000);
    }
    if (appId == "stocks" && app.containsKey("stockDurationMs")) {
      Firebase.RTDB.setInt(
        &configFbdo,
        (getDeviceAppPath(appId) + "/symbolDurationMs").c_str(),
        app["stockDurationMs"].as<int>());
      Firebase.RTDB.deleteNode(&configFbdo, (getDeviceAppPath(appId) + "/stockDurationMs").c_str());
    }
    if (appId == "stocks" && !app.containsKey("valueMode")) {
      Firebase.RTDB.setString(&configFbdo, (getDeviceAppPath(appId) + "/valueMode").c_str(), "dollar");
    }
  }
}

void migrateLocationSettings(bool allowLocationLookup) {
  String settingsPath = getSettingsPath();
  if (!Firebase.RTDB.getJSON(&configFbdo, settingsPath.c_str())) {
    return;
  }

  String jsonStr;
  configFbdo.jsonObject().toString(jsonStr, true);
  StaticJsonDocument<4096> doc;
  if (deserializeJson(doc, jsonStr)) {
    return;
  }

  JsonObject location = doc["location"].as<JsonObject>();
  bool hasLocationObject = !location.isNull();

  float lat = hasLocationObject ? (location["lat"] | 0.0f) : (doc["lat"] | 0.0f);
  float lon = hasLocationObject ? (location["lon"] | 0.0f) : (doc["lon"] | 0.0f);
  String label = hasLocationObject ? String(location["label"] | "") : String(doc["weatherLocation"] | "");
  String timezone = String(doc["timezone"] | "");

  if ((lat == 0.0f || lon == 0.0f || label.length() == 0 || timezone.length() == 0) && allowLocationLookup) {
    float fetchedLat = lat;
    float fetchedLon = lon;
    String fetchedLabel = label;
    String fetchedTimezone = timezone;
    if (fetchGeoLocation(fetchedLat, fetchedLon, fetchedLabel, fetchedTimezone)) {
      if (lat == 0.0f) lat = fetchedLat;
      if (lon == 0.0f) lon = fetchedLon;
      if (label.length() == 0) label = fetchedLabel;
      if (timezone.length() == 0) timezone = fetchedTimezone;
    }
  }

  FirebaseJson json;
  json.set("location/lat", lat);
  json.set("location/lon", lon);
  json.set("location/label", label);
  json.set("timezone", timezone);
  Firebase.RTDB.updateNode(&configFbdo, settingsPath.c_str(), &json);

  const char* legacyKeys[] = {"lat", "lon", "weatherLocation", "placeholder"};
  for (const char* key : legacyKeys) {
    Firebase.RTDB.deleteNode(&configFbdo, (settingsPath + "/" + key).c_str());
  }
}

void seedCanonicalApps() {
  for (const auto& app : kCanonicalApps) {
    seedAppIfMissing(app);
  }
}

void seedSleepScheduleIfMissing() {
  for (size_t i = 0; i < kDayCount; ++i) {
    String base = getSettingsPath() + "/sleepSchedule/" + kDayKeys[i];
    if (!Firebase.RTDB.getJSON(&configFbdo, base.c_str())) {
      FirebaseJson json;
      json.set("enabled", false);
      json.set("wake", "07:00");
      json.set("sleep", "23:00");
      Firebase.RTDB.updateNode(&configFbdo, base.c_str(), &json);
    }
  }
}

int parseMinuteString(const String& value) {
  if (value.length() != 5 || value[2] != ':') return -1;
  int hours = value.substring(0, 2).toInt();
  int minutes = value.substring(3, 5).toInt();
  if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59) return -1;
  return hours * 60 + minutes;
}

bool ensureStream(FirebaseData& streamFbdo, const String& path, bool& started, unsigned long& lastRetryAt, const char* label) {
  if (started) return true;
  if (millis() - lastRetryAt < kConfigStreamRetryMs) return false;

  lastRetryAt = millis();
  firebaseTelemetry.streamBeginAttempts++;
  streamFbdo.setResponseSize(32768);
  Firebase.RTDB.setReadTimeout(&streamFbdo, 30000);
  Firebase.RTDB.setMaxRetry(&streamFbdo, 3);
  Firebase.RTDB.enableClassicRequest(&streamFbdo, true);
  started = Firebase.RTDB.beginStream(&streamFbdo, path.c_str());
  if (started) {
    firebaseTelemetry.streamBeginSuccess++;
    consecutiveStreamReadFailures = 0;
    Serial.println(String("✅ Firebase stream ready: ") + label);
    if (String(label) == "device") {
      refreshDeviceStocksRuntimeSnapshot(false);
    }
  } else {
    Serial.println(String("⚠️ Failed to start Firebase stream for ") + label + ": " + streamFbdo.errorReason());
    streamFbdo.stopWiFiClient();
    Firebase.reconnectWiFi(true);
  }
  return started;
}

std::vector<String> buildMigratedSequence() {
  std::vector<String> nextSequence;

  for (const String& appId : deviceSettings.appSequence) {
    if (appId.length() == 0) continue;
    if (appId == "sports") continue;
    if (std::find(nextSequence.begin(), nextSequence.end(), appId) == nextSequence.end()) {
      nextSequence.push_back(appId);
    }
  }

  const char* canonicalOrder[] = {"clockWeather", "forecast", "stocks", "mlb", "nba", "nfl", "nhl", "clock", "weather"};
  for (const char* rawId : canonicalOrder) {
    String appId(rawId);
    auto it = deviceAppConfigs.find(appId);
    if (it == deviceAppConfigs.end() || !it->second.enabled) continue;
    if (std::find(nextSequence.begin(), nextSequence.end(), appId) == nextSequence.end()) {
      nextSequence.push_back(appId);
    }
  }

  for (const auto& entry : deviceAppConfigs) {
    if (!entry.second.enabled) continue;
    if (std::find(nextSequence.begin(), nextSequence.end(), entry.first) == nextSequence.end()) {
      nextSequence.push_back(entry.first);
    }
  }

  return nextSequence;
}

void migrateIncompleteSequenceIfNeeded() {
  std::vector<String> nextSequence = buildMigratedSequence();
  if (stringVectorsEqual(nextSequence, deviceSettings.appSequence)) {
    return;
  }

  FirebaseJsonArray seq;
  for (const String& appId : nextSequence) {
    seq.add(appId);
  }
  Firebase.RTDB.setArray(&configFbdo, (getSettingsPath() + "/appSequence").c_str(), &seq);
}

uint32_t diffSettings(const DeviceSettings& previous, const DeviceSettings& next) {
  uint32_t flags = DeviceConfigChangeNone;

  if (previous.brightness != next.brightness) flags |= DeviceConfigChangeBrightness;
  if (previous.timeFormat != next.timeFormat) flags |= DeviceConfigChangeTimeFormat;
  if (previous.units != next.units) flags |= DeviceConfigChangeUnits;
  if (!stringVectorsEqual(previous.appSequence, next.appSequence)) flags |= DeviceConfigChangeSequence;
  if (previous.timezone != next.timezone ||
      previous.autoLocation != next.autoLocation ||
      previous.locationLat != next.locationLat ||
      previous.locationLon != next.locationLon ||
      previous.locationLabel != next.locationLabel) {
    flags |= DeviceConfigChangeLocation;
  }

  for (size_t i = 0; i < kDayCount; ++i) {
    if (!dayScheduleEqual(previous.sleepSchedule[i], next.sleepSchedule[i])) {
      flags |= DeviceConfigChangeSleepSchedule;
      break;
    }
  }

  return flags;
}

uint32_t diffApps(const std::map<String, AppConfig>& previous, const std::map<String, AppConfig>& next) {
  uint32_t flags = DeviceConfigChangeNone;
  std::vector<String> keys;
  for (const auto& entry : previous) keys.push_back(entry.first);
  for (const auto& entry : next) {
    if (std::find(keys.begin(), keys.end(), entry.first) == keys.end()) {
      keys.push_back(entry.first);
    }
  }

  for (const String& key : keys) {
    auto prevIt = previous.find(key);
    auto nextIt = next.find(key);
    bool prevExists = prevIt != previous.end();
    bool nextExists = nextIt != next.end();
    if (!prevExists || !nextExists) {
      flags |= DeviceConfigChangeAppState;
      if (key == "stocks") flags |= DeviceConfigChangeStocks;
      if (isSportsAppId(key)) flags |= DeviceConfigChangeSports;
      continue;
    }

    const AppConfig& prevConfig = prevIt->second;
    const AppConfig& nextConfig = nextIt->second;
    if (prevConfig.enabled != nextConfig.enabled || prevConfig.durationMs != nextConfig.durationMs) {
      flags |= DeviceConfigChangeAppState;
    }
    if (key == "stocks") {
      if (!stringVectorsEqual(prevConfig.symbols, nextConfig.symbols) ||
          prevConfig.valueMode != nextConfig.valueMode ||
          prevConfig.symbolDurationMs != nextConfig.symbolDurationMs) {
        flags |= DeviceConfigChangeStocks;
      }
    }
    if (isSportsAppId(key) && !favoritesEqual(prevConfig.favorites, nextConfig.favorites)) {
      flags |= DeviceConfigChangeSports;
    }
  }

  return flags;
}

}  // namespace

bool initializeDeviceConfig(bool allowLocationLookup) {
  if (!Firebase.ready()) {
    Serial.println("⚠️ Firebase not ready. Cannot initialize device config.");
    return false;
  }

  int existingSchemaVersion = 0;
  if (Firebase.RTDB.getInt(&configFbdo, (getStatusPath() + "/schemaVersion").c_str())) {
    existingSchemaVersion = configFbdo.intData();
  }

  ensureCatalogSchema();
  cleanupCatalogDurations();

  bool settingsExists = Firebase.RTDB.getJSON(&configFbdo, getSettingsPath().c_str());
  seedDefaultSettings(settingsExists);
  if (existingSchemaVersion < kSchemaVersion) {
    migrateCoreSettings();
    migrateLegacyStockConfig();
    migratePerAppDurations();
  }
  seedCanonicalApps();
  migrateLegacySportsConfig();
  seedSleepScheduleIfMissing();
  migrateLocationSettings(allowLocationLookup);

  if (!refreshDeviceConfig(true)) {
    return false;
  }

  if (existingSchemaVersion < kSchemaVersion) {
    migrateIncompleteSequenceIfNeeded();
    if (!refreshDeviceConfig(true)) {
      return false;
    }
  }

  deviceStreamStarted = false;
  lastDeviceStreamRetryAt = 0;
  consecutiveStreamReadFailures = 0;
  clearDeviceStocksRuntime();
  refreshDeviceStocksRuntimeSnapshot(false);

  Firebase.RTDB.setInt(&configFbdo, (getStatusPath() + "/schemaVersion").c_str(), kSchemaVersion);
  return true;
}

bool refreshDeviceConfig(bool forceRefresh) {
  (void)forceRefresh;
  firebaseTelemetry.configRefreshCalls++;

  if (!Firebase.ready()) {
    firebaseTelemetry.configRefreshFailures++;
    return false;
  }

  DynamicJsonDocument settingsDoc(8192);
  DynamicJsonDocument appsDoc(8192);
  if (!readJsonAtPath(configFbdo, getSettingsPath(), settingsDoc)) {
    firebaseTelemetry.configRefreshFailures++;
    Serial.println("❌ Failed to load device settings.");
    return false;
  }
  if (!readJsonAtPath(configFbdo, deviceRoot() + "/apps", appsDoc)) {
    firebaseTelemetry.configRefreshFailures++;
    Serial.println("❌ Failed to load device apps.");
    return false;
  }

  DeviceSettings nextSettings;
  nextSettings.brightness = constrain(settingsDoc["brightness"] | 7, 1, 10);
  nextSettings.timeFormat = constrain(settingsDoc["timeFormat"] | 1, 0, 2);
  nextSettings.units = String(settingsDoc["units"] | "metric");
  nextSettings.autoLocation = settingsDoc["autoLocation"] | true;
  nextSettings.timezone = String(settingsDoc["timezone"] | "");
  nextSettings.locationLat = settingsDoc["location"]["lat"] | 0.0f;
  nextSettings.locationLon = settingsDoc["location"]["lon"] | 0.0f;
  nextSettings.locationLabel = String(settingsDoc["location"]["label"] | "");

  JsonArray sequence = settingsDoc["appSequence"].as<JsonArray>();
  if (!sequence.isNull()) {
    for (JsonVariant v : sequence) {
      const char* rawId = v.as<const char*>();
      String appId = normalizeAppId(rawId == nullptr ? "" : String(rawId));
      if (appId.length() > 0) {
        nextSettings.appSequence.push_back(appId);
      }
    }
  }
  if (nextSettings.appSequence.empty()) {
    nextSettings.appSequence = {"clockWeather", "forecast", "stocks"};
  }

  for (size_t i = 0; i < kDayCount; ++i) {
    JsonObject day = settingsDoc["sleepSchedule"][kDayKeys[i]].as<JsonObject>();
    nextSettings.sleepSchedule[i].enabled = day["enabled"] | false;
    nextSettings.sleepSchedule[i].wake = String(day["wake"] | "07:00");
    nextSettings.sleepSchedule[i].sleep = String(day["sleep"] | "23:00");
  }

  std::map<String, AppConfig> nextApps;
  for (JsonPair kv : appsDoc.as<JsonObject>()) {
    String appId = normalizeAppId(String(kv.key().c_str()));
    JsonObject app = kv.value().as<JsonObject>();

    AppConfig config;
    config.enabled = app["enabled"] | false;
    config.durationMs = app["durationMs"] | 0UL;
    if (appId == "stocks") {
      String mode = String(app["valueMode"] | "dollar");
      mode.toLowerCase();
      config.valueMode = (mode == "percent") ? "percent" : "dollar";
      config.symbolDurationMs = app["symbolDurationMs"] | 0UL;
      if (config.symbolDurationMs == 0UL) {
        config.symbolDurationMs = app["stockDurationMs"] | 5000UL;
      }
      config.symbolDurationMs = constrain(config.symbolDurationMs, 1500UL, 60000UL);
    }
    if (config.durationMs == 0UL) {
      config.durationMs = app["duration"] | 10000UL;
    }

    JsonArray symbols = app["symbols"].as<JsonArray>();
    if (symbols.isNull()) {
      symbols = app["symbols"]["symbols"].as<JsonArray>();
    }
    if (!symbols.isNull()) {
      for (JsonVariant v : symbols) {
        const char* rawSymbol = v.as<const char*>();
        if (rawSymbol != nullptr && strlen(rawSymbol) > 0) {
          config.symbols.push_back(String(rawSymbol));
        }
      }
    }

    JsonArray favorites = app["favorites"].as<JsonArray>();
    if (!favorites.isNull()) {
      for (JsonVariant v : favorites) {
        TeamFavorite favorite;
        favorite.league = String(v["league"] | "");
        if (favorite.league.length() == 0 && isSportsAppId(appId) && appId != "sports") {
          favorite.league = appId;
        }
        favorite.teamId = String(v["teamId"] | "");
        favorite.league.toLowerCase();
        favorite.teamId.toUpperCase();
        if (favorite.league.length() > 0 && favorite.teamId.length() > 0) {
          config.favorites.push_back(favorite);
        }
      }
    }

    nextApps[appId] = config;
  }

  uint32_t flags = diffSettings(deviceSettings, nextSettings) | diffApps(deviceAppConfigs, nextApps);

  deviceSettings = nextSettings;
  deviceAppConfigs = nextApps;

  units = deviceSettings.units;
  timeFormatPreference = deviceSettings.timeFormat;
  storedLat = deviceSettings.locationLat;
  storedLon = deviceSettings.locationLon;
  pendingConfigChangeFlags |= flags;
  return true;
}

bool pollDeviceConfigStreams() {
  if (!Firebase.ready()) return false;

  ensureStream(deviceStreamFbdo, deviceRoot(), deviceStreamStarted, lastDeviceStreamRetryAt, "device");

  if (!deviceStreamStarted) {
    return false;
  }

  firebaseTelemetry.streamReadCalls++;
  if (!Firebase.RTDB.readStream(&deviceStreamFbdo)) {
    firebaseTelemetry.streamReadFalse++;
    if (deviceStreamFbdo.streamTimeout()) {
      firebaseTelemetry.streamTimeouts++;
      return false;
    }

    String reason = deviceStreamFbdo.errorReason();
    reason.trim();
    if (reason.length() == 0 || !hasActionableStreamError(reason)) {
      // The Firebase client occasionally reports transient false reads with
      // no actionable reason. Keep the stream alive and wait for next event.
      consecutiveStreamReadFailures = 0;
      return false;
    }

    ++consecutiveStreamReadFailures;
    if (consecutiveStreamReadFailures < 3) {
      return false;
    }

    consecutiveStreamReadFailures = 0;
    deviceStreamStarted = false;
    firebaseTelemetry.streamDrops++;
    Serial.println("⚠️ Device stream dropped: " + reason);
    deviceStreamFbdo.stopWiFiClient();
    Firebase.reconnectWiFi(true);
    return false;
  }

  consecutiveStreamReadFailures = 0;

  if (!deviceStreamFbdo.streamAvailable()) {
    return false;
  }
  firebaseTelemetry.streamEvents++;

  String dataPath = deviceStreamFbdo.dataPath();
  if (dataPath.startsWith("/status")) {
    return false;
  }

  bool inspectRuntimePayload = dataPath == "/" ||
                               dataPath == "/runtime" ||
                               dataPath == "/runtime/stocks" ||
                               dataPath.startsWith("/runtime/stocks/");

  if (inspectRuntimePayload) {
    DynamicJsonDocument streamDoc(24576);
    bool hasPayload = parseStreamPayload(deviceStreamFbdo, streamDoc);
    if (hasPayload) {
      if (dataPath == "/runtime/stocks") {
        JsonObjectConst stocksRoot = streamDoc.as<JsonObjectConst>();
        applyStockRuntimeJson(stocksRoot);
        return true;
      }
      if (dataPath == "/" || dataPath == "/runtime") {
        JsonObjectConst root = streamDoc.as<JsonObjectConst>();
        if (!root["runtime"]["stocks"].isNull()) {
          applyStockRuntimeJson(root["runtime"]["stocks"].as<JsonObjectConst>());
        } else if (!root["stocks"].isNull() && dataPath == "/runtime") {
          applyStockRuntimeJson(root["stocks"].as<JsonObjectConst>());
        }
      }
      if (dataPath.startsWith("/runtime/stocks/")) {
        return false;
      }
    }
  }

  if (dataPath.startsWith("/runtime")) {
    return false;
  }

  if (dataPath == "/settings/brightness") {
    int nextBrightness = deviceSettings.brightness;
    if (parseStreamIntValue(deviceStreamFbdo, nextBrightness)) {
      nextBrightness = constrain(nextBrightness, 1, 10);
      if (nextBrightness != deviceSettings.brightness) {
        deviceSettings.brightness = nextBrightness;
        pendingConfigChangeFlags |= DeviceConfigChangeBrightness;
      }
      return true;
    }
  }

  if (dataPath == "/settings/location/label") {
    String nextLabel = deviceSettings.locationLabel;
    if (!parseStreamStringValue(deviceStreamFbdo, nextLabel)) return false;
    nextLabel.trim();
    if (nextLabel != deviceSettings.locationLabel) {
      deviceSettings.locationLabel = nextLabel;
      pendingConfigChangeFlags |= DeviceConfigChangeLocation;
    }
    return true;
  }

  if (dataPath == "/settings/location/lat") {
    float nextLat = deviceSettings.locationLat;
    if (parseStreamFloatValue(deviceStreamFbdo, nextLat) && nextLat != deviceSettings.locationLat) {
      deviceSettings.locationLat = nextLat;
      storedLat = nextLat;
      pendingConfigChangeFlags |= DeviceConfigChangeLocation;
    }
    return true;
  }

  if (dataPath == "/settings/location/lon") {
    float nextLon = deviceSettings.locationLon;
    if (parseStreamFloatValue(deviceStreamFbdo, nextLon) && nextLon != deviceSettings.locationLon) {
      deviceSettings.locationLon = nextLon;
      storedLon = nextLon;
      pendingConfigChangeFlags |= DeviceConfigChangeLocation;
    }
    return true;
  }

  if (dataPath == "/settings/timezone") {
    String nextTimezone = deviceSettings.timezone;
    if (!parseStreamStringValue(deviceStreamFbdo, nextTimezone)) return false;
    nextTimezone.trim();
    if (nextTimezone != deviceSettings.timezone) {
      deviceSettings.timezone = nextTimezone;
      pendingConfigChangeFlags |= DeviceConfigChangeLocation;
    }
    return true;
  }

  if (dataPath == "/settings/autoLocation") {
    bool nextAutoLocation = deviceSettings.autoLocation;
    if (parseStreamBoolValue(deviceStreamFbdo, nextAutoLocation)) {
      if (nextAutoLocation != deviceSettings.autoLocation) {
        deviceSettings.autoLocation = nextAutoLocation;
      }
      if (deviceSettings.autoLocation) {
        maybeAutoUpdateLocationFromGeo(true);
      }
      return true;
    }
  }

  if (dataPath == "/settings/appSequence") {
    DynamicJsonDocument streamDoc(2048);
    if (parseStreamPayload(deviceStreamFbdo, streamDoc)) {
      std::vector<String> nextSequence;
      JsonArrayConst seq = streamDoc.as<JsonArrayConst>();
      for (JsonVariantConst item : seq) {
        const char* rawId = item.as<const char*>();
        String appId = normalizeAppId(rawId == nullptr ? "" : String(rawId));
        if (appId.length() == 0) continue;
        nextSequence.push_back(appId);
      }
      if (!nextSequence.empty() && !stringVectorsEqual(nextSequence, deviceSettings.appSequence)) {
        deviceSettings.appSequence = nextSequence;
        pendingConfigChangeFlags |= DeviceConfigChangeSequence;
      }
      return true;
    }
  }

  if (dataPath.startsWith("/settings/appSequence/")) {
    int index = dataPath.substring(strlen("/settings/appSequence/")).toInt();
    if (index < 0) return false;
    String rawValue;
    if (!parseStreamStringValue(deviceStreamFbdo, rawValue)) return false;
    String appId = normalizeAppId(rawValue);
    if (appId.length() == 0) return true;

    if (static_cast<size_t>(index) >= deviceSettings.appSequence.size()) {
      deviceSettings.appSequence.resize(static_cast<size_t>(index) + 1);
    }

    if (deviceSettings.appSequence[static_cast<size_t>(index)] != appId) {
      deviceSettings.appSequence[static_cast<size_t>(index)] = appId;
      pendingConfigChangeFlags |= DeviceConfigChangeSequence;
    }
    return true;
  }

  if (dataPath.startsWith("/apps/")) {
    int secondSlash = dataPath.indexOf('/', 6);
    if (secondSlash > 6) {
      String appId = normalizeAppId(dataPath.substring(6, secondSlash));
      String field = dataPath.substring(secondSlash + 1);
      auto it = deviceAppConfigs.find(appId);
      if (it == deviceAppConfigs.end()) {
        deviceAppConfigs[appId] = AppConfig();
        it = deviceAppConfigs.find(appId);
      }

      if (field == "enabled") {
        bool nextEnabled = it->second.enabled;
        if (parseStreamBoolValue(deviceStreamFbdo, nextEnabled) && nextEnabled != it->second.enabled) {
          it->second.enabled = nextEnabled;
          pendingConfigChangeFlags |= DeviceConfigChangeAppState;
        }
        return true;
      }

      if (field == "durationMs") {
        unsigned long nextDuration = it->second.durationMs;
        if (parseStreamUnsignedLongValue(deviceStreamFbdo, nextDuration)) {
          nextDuration = constrain(nextDuration, 1000UL, 120000UL);
          if (nextDuration != it->second.durationMs) {
            it->second.durationMs = nextDuration;
            pendingConfigChangeFlags |= DeviceConfigChangeAppState;
          }
        }
        return true;
      }

      if (appId == "stocks" && field == "symbolDurationMs") {
        unsigned long nextDuration = it->second.symbolDurationMs;
        if (parseStreamUnsignedLongValue(deviceStreamFbdo, nextDuration)) {
          nextDuration = constrain(nextDuration, 1500UL, 60000UL);
          if (nextDuration != it->second.symbolDurationMs) {
            it->second.symbolDurationMs = nextDuration;
            pendingConfigChangeFlags |= DeviceConfigChangeStocks;
          }
        }
        return true;
      }

      if (appId == "stocks" && field == "valueMode") {
        String mode = deviceStreamFbdo.stringData();
        mode.trim();
        mode.toLowerCase();
        String nextMode = (mode == "percent") ? "percent" : "dollar";
        if (nextMode != it->second.valueMode) {
          it->second.valueMode = nextMode;
          pendingConfigChangeFlags |= DeviceConfigChangeStocks;
        }
        return true;
      }
    }
  }

  return refreshDeviceConfig(true);
}

uint32_t takeDeviceConfigChangeFlags() {
  uint32_t flags = pendingConfigChangeFlags;
  pendingConfigChangeFlags = DeviceConfigChangeNone;
  return flags;
}

const AppConfig* getAppConfig(const String& appId) {
  auto it = deviceAppConfigs.find(normalizeAppId(appId));
  return it == deviceAppConfigs.end() ? nullptr : &it->second;
}

unsigned long getAppDurationMs(const String& appId) {
  const AppConfig* config = getAppConfig(appId);
  return config ? config->durationMs : 10000;
}

bool isAppEnabled(const String& appId) {
  const AppConfig* config = getAppConfig(appId);
  return config ? config->enabled : false;
}

const DeviceStocksRuntime& getDeviceStocksRuntime() {
  return deviceStocksRuntime;
}

bool getStockRuntimeEntry(const String& symbol, StockRuntimeEntry& outEntry) {
  String normalized = normalizeSymbol(symbol);
  auto it = deviceStocksRuntime.entries.find(normalized);
  if (it == deviceStocksRuntime.entries.end()) {
    return false;
  }
  outEntry = it->second;
  return true;
}

void clearDeviceStocksRuntime() {
  deviceStocksRuntime = DeviceStocksRuntime();
}

bool shouldSleepNow(int weekdayIndex, int minutesSinceMidnight) {
  if (weekdayIndex < 0 || weekdayIndex >= static_cast<int>(kDayCount) || minutesSinceMidnight < 0) {
    return false;
  }

  const DaySleepSchedule& day = deviceSettings.sleepSchedule[weekdayIndex];
  if (!day.enabled) return false;

  int wakeMinutes = parseMinuteString(day.wake);
  int sleepMinutes = parseMinuteString(day.sleep);
  if (wakeMinutes < 0 || sleepMinutes < 0 || wakeMinutes == sleepMinutes) {
    return false;
  }

  if (wakeMinutes < sleepMinutes) {
    return minutesSinceMidnight < wakeMinutes || minutesSinceMidnight >= sleepMinutes;
  }

  return minutesSinceMidnight >= sleepMinutes && minutesSinceMidnight < wakeMinutes;
}

void writeDeviceStatus(const String& activeAppId, bool isSleeping, unsigned long unixTime, bool timeSynchronized) {
  static unsigned long lastWrite = 0;
  static bool lastSleeping = false;
  static bool lastTimeSyncOk = false;
  const unsigned long writeIntervalMs = 300000;

  bool heartbeatDue = millis() - lastWrite >= writeIntervalMs;
  bool stateChanged = isSleeping != lastSleeping ||
                      timeSynchronized != lastTimeSyncOk;

  if ((!heartbeatDue && !stateChanged) || !Firebase.ready()) {
    return;
  }

  FirebaseJson status;
  status.set("online", 1);
  status.set("activeApp", activeAppId);
  status.set("isSleeping", isSleeping ? 1 : 0);
  status.set("timeSyncOk", timeSynchronized ? 1 : 0);
  if (timeSynchronized && unixTime > 0) {
    status.set("lastSeenAt", static_cast<int>(unixTime));
  }
  status.set("schemaVersion", kSchemaVersion);
  status.set("wifiRssi", static_cast<int>(WiFi.RSSI()));
  status.set("firmwareVersion", SecretsManager::get("CURRENT_VERSION"));
  status.set("firebaseStats/uptimeSec", static_cast<int>(millis() / 1000UL));
  status.set("firebaseStats/streamBeginAttempts", static_cast<int>(firebaseTelemetry.streamBeginAttempts));
  status.set("firebaseStats/streamBeginSuccess", static_cast<int>(firebaseTelemetry.streamBeginSuccess));
  status.set("firebaseStats/streamReadCalls", static_cast<int>(firebaseTelemetry.streamReadCalls));
  status.set("firebaseStats/streamReadFalse", static_cast<int>(firebaseTelemetry.streamReadFalse));
  status.set("firebaseStats/streamTimeouts", static_cast<int>(firebaseTelemetry.streamTimeouts));
  status.set("firebaseStats/streamEvents", static_cast<int>(firebaseTelemetry.streamEvents));
  status.set("firebaseStats/streamDrops", static_cast<int>(firebaseTelemetry.streamDrops));
  status.set("firebaseStats/runtimeSnapshotRefreshCalls", static_cast<int>(firebaseTelemetry.runtimeSnapshotRefreshCalls));
  status.set("firebaseStats/runtimeSnapshotRefreshFailures", static_cast<int>(firebaseTelemetry.runtimeSnapshotRefreshFailures));
  status.set("firebaseStats/runtimeSnapshotApplies", static_cast<int>(firebaseTelemetry.runtimeSnapshotApplies));
  status.set("firebaseStats/configRefreshCalls", static_cast<int>(firebaseTelemetry.configRefreshCalls));
  status.set("firebaseStats/configRefreshFailures", static_cast<int>(firebaseTelemetry.configRefreshFailures));
  status.set("firebaseStats/statusWrites", static_cast<int>(firebaseTelemetry.statusWrites + 1));
  Firebase.RTDB.updateNode(&statusFbdo, getStatusPath().c_str(), &status);
  firebaseTelemetry.statusWrites++;
  lastWrite = millis();
  lastSleeping = isSleeping;
  lastTimeSyncOk = timeSynchronized;
}

bool maybeAutoUpdateLocationFromGeo(bool force) {
  if (!deviceSettings.autoLocation && !force) {
    return false;
  }
  if (!Firebase.ready()) {
    return false;
  }
  unsigned long now = millis();
  if (!force && (now - lastAutoLocationSyncAt) < kAutoLocationIntervalMs) {
    return false;
  }
  lastAutoLocationSyncAt = now;

  float fetchedLat = 0.0f;
  float fetchedLon = 0.0f;
  String fetchedLabel;
  String fetchedTimezone;
  if (!fetchGeoLocation(fetchedLat, fetchedLon, fetchedLabel, fetchedTimezone)) {
    return false;
  }

  bool changed = false;
  if (fabsf(fetchedLat - deviceSettings.locationLat) > 0.02f) changed = true;
  if (fabsf(fetchedLon - deviceSettings.locationLon) > 0.02f) changed = true;

  fetchedLabel.trim();
  if (fetchedLabel.length() > 0 && fetchedLabel != deviceSettings.locationLabel) changed = true;

  fetchedTimezone.trim();
  if (fetchedTimezone.length() > 0 && fetchedTimezone != deviceSettings.timezone) changed = true;

  if (!changed) {
    return false;
  }

  FirebaseJson json;
  json.set("location/lat", fetchedLat);
  json.set("location/lon", fetchedLon);
  json.set("location/label", fetchedLabel);
  json.set("timezone", fetchedTimezone);
  if (!Firebase.RTDB.updateNode(&configFbdo, getSettingsPath().c_str(), &json)) {
    Serial.println("⚠️ Auto-location update failed: " + configFbdo.errorReason());
    return false;
  }

  deviceSettings.locationLat = fetchedLat;
  deviceSettings.locationLon = fetchedLon;
  deviceSettings.locationLabel = fetchedLabel;
  if (fetchedTimezone.length() > 0) {
    deviceSettings.timezone = fetchedTimezone;
  }
  storedLat = fetchedLat;
  storedLon = fetchedLon;
  pendingConfigChangeFlags |= DeviceConfigChangeLocation;
  Serial.println("📍 Auto-location updated from current network.");
  return true;
}
