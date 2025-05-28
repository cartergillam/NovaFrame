#include "TimeCache.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "secrets.h"
#include "DeviceRegistration.h"  // for storedLat, storedLon, timeFormatPreference

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

  HTTPClient http;
  String query = "https://api.ipgeolocation.io/timezone?apiKey=" + String(IP_GEO_LOCATION_API_KEY) +
                 "&lat=" + String(storedLat, 4) + "&long=" + String(storedLon, 4);
  http.begin(query);
  Serial.println("Time zone query: " + query);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      Serial.print("❌ JSON parse error: ");
      Serial.println(err.c_str());
      return;
    }

    String date = doc["date"];           // "2025-05-22"
    String time24 = doc["time_24"];      // "16:19:21"
    String isoTime = date + "T" + time24;

    Serial.println("✅ ISO datetime composed: " + isoTime);

    struct tm t;
    memset(&t, 0, sizeof(t));
    strptime(isoTime.c_str(), "%Y-%m-%dT%H:%M:%S", &t);

    baseEpoch = mktime(&t);
    epochStartMillis = millis();

    Serial.printf("🕒 Epoch set to: %ld\n", baseEpoch);
  } else {
    Serial.println("❌ Time API failed: " + http.errorToString(code));
  }

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