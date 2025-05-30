#include "TimeCache.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "DeviceRegistration.h"  // for storedLat, storedLon, timeFormatPreference
#include "RemoteConfigManager.h"

extern int timeFormatPreference;  // 0 = 12hr no AM/PM, 1 = 12hr with AM/PM, 24 = 24hr
extern float storedLat;
extern float storedLon;

void TimeCache::init() {
  fetchTime();
  lastSync = millis();
}

void TimeCache::updateIfNeeded() {
  if (millis() - lastSync > SYNC_INTERVAL) {
    fetchTime();
    lastSync = millis();
  }
}

void TimeCache::fetchTime() {
  
  if (storedLat == 0.0 || storedLon == 0.0) {
    Serial.println("⚠️ Lat/Lon not set, skipping time fetch");
    return;
  }
  if (!Firebase.ready()) {
    Serial.println("⚠️ Firebase not ready — skipping time fetch.");
    return;
  }
  String apiKey = RemoteConfigManager::get("IP_GEO_LOCATION_API_KEY", "");
  if (apiKey == "") {
    Serial.println("❌ API key for timezone not found in secrets.");
    return;
  }

  HTTPClient http;
  String query = "https://api.ipgeolocation.io/timezone?apiKey=" + apiKey +
                 "&lat=" + String(storedLat, 4) + "&long=" + String(storedLon, 4);
  http.begin(query);
  Serial.println("🌐 Time zone query: " + query);

  int code = http.GET();
  if (code != 200) {
    Serial.println("❌ Time API failed: " + http.errorToString(code));
    http.end();
    return;
  }

  String payload = http.getString();

  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("❌ JSON parse error: ");
    Serial.println(err.c_str());
    http.end();
    return;
  }

  // Try multiple keys to support both formats
  String date = doc["date"] | "";
  String time24 = doc["time_24"] | doc["time_24hr"] | "";
  String isoTime = "";

  if (doc.containsKey("date_time_24")) {
    isoTime = doc["date_time_24"].as<String>();
    Serial.println("✅ Using date_time_24 field.");
  } else if (date != "" && time24 != "") {
    isoTime = date + "T" + time24;
    Serial.println("✅ Composed ISO time from date + time_24.");
  } else {
    Serial.println("❌ No valid date/time fields found in API response.");
    http.end();
    return;
  }

  Serial.println("🧪 Composed ISO datetime: " + isoTime);

  struct tm t;
  memset(&t, 0, sizeof(t));
  if (!strptime(isoTime.c_str(), "%Y-%m-%dT%H:%M:%S", &t)) {
    Serial.println("❌ strptime failed. Could not parse time string.");
    http.end();
    return;
  }

  baseEpoch = mktime(&t);
  epochStartMillis = millis();

  Serial.printf("✅ Parsed Time: %04d-%02d-%02d %02d:%02d:%02d\n",
                t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                t.tm_hour, t.tm_min, t.tm_sec);
  Serial.printf("🕒 Epoch set to: %ld\n", baseEpoch);

  http.end();
}

String TimeCache::getCurrentTimeString() {
  time_t now = baseEpoch + ((millis() - epochStartMillis) / 1000);
  struct tm* t = localtime(&now);
  char buf[9];
  sprintf(buf, "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
  return String(buf);
}

int TimeCache::getHour() {
  time_t now = baseEpoch + ((millis() - epochStartMillis) / 1000);
  struct tm* t = localtime(&now);
  return t->tm_hour;
}

int TimeCache::getMinute() {
  time_t now = baseEpoch + ((millis() - epochStartMillis) / 1000);
  struct tm* t = localtime(&now);
  return t->tm_min;
}

String TimeCache::getFormattedTime() {
  int h = getHour();
  int m = getMinute();
  String suffix = "";

  if (timeFormatPreference == 0 || timeFormatPreference == 1) {
    // Convert to 12-hour format
    suffix = (timeFormatPreference == 1) ? ((h >= 12) ? "PM" : "AM") : "";
    h = h % 12;
    if (h == 0) h = 12;
  }

  char buffer[8];
  snprintf(buffer, sizeof(buffer), "%d:%02d", h, m);  // ✅ Drop leading zero from hour
  String timeStr = String(buffer);

  if (timeFormatPreference == 1) {
    timeStr += suffix;
  }

  return timeStr;
}