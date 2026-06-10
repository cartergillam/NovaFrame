#include "TimeCache.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "DeviceRegistration.h"  // for storedLat, storedLon, timeFormatPreference
#include "DeviceConfig.h"
#include "RemoteConfigManager.h"

extern int timeFormatPreference;  // 0 = 12hr, 1 = 12hr + AM/PM, 2 = 24hr
extern float storedLat;
extern float storedLon;

namespace {

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

double jsonToDouble(JsonVariant value, double fallback = 0.0) {
  if (value.isNull()) return fallback;
  if (value.is<float>() || value.is<double>()) return value.as<double>();
  if (value.is<int>() || value.is<long>()) return static_cast<double>(value.as<long>());
  const char* raw = value.as<const char*>();
  if (raw != nullptr && strlen(raw) > 0) return atof(raw);
  return fallback;
}

unsigned long jsonToUnsignedLong(JsonVariant value, unsigned long fallback = 0UL) {
  if (value.isNull()) return fallback;
  if (value.is<int>() || value.is<long>()) {
    long parsed = value.as<long>();
    return parsed > 0 ? static_cast<unsigned long>(parsed) : fallback;
  }
  if (value.is<float>() || value.is<double>()) {
    double parsed = value.as<double>();
    return parsed > 0.0 ? static_cast<unsigned long>(parsed) : fallback;
  }
  const char* raw = value.as<const char*>();
  if (raw != nullptr && strlen(raw) > 0) {
    unsigned long parsed = strtoul(raw, nullptr, 10);
    return parsed > 0 ? parsed : fallback;
  }
  return fallback;
}

bool parseYmdHms(const String& value, int& year, int& month, int& day, int& hour, int& minute, int& second) {
  return sscanf(value.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6 ||
         sscanf(value.c_str(), "%d/%d/%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6;
}

long long daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned mp = month > 2 ? month - 3 : month + 9;
  const unsigned doy = (153 * mp + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097LL + static_cast<long long>(doe) - 719468LL;
}

bool parseUnixFromLocalDateTime(const DynamicJsonDocument& doc, long utcOffsetSeconds, unsigned long& unixTime) {
  String dateTime = String(doc["date_time"] | "");
  if (dateTime.length() == 0) {
    String date = String(doc["date"] | "");
    String time24 = String(doc["time_24"] | "");
    if (date.length() > 0 && time24.length() > 0) {
      dateTime = date + " " + time24;
    }
  }

  if (dateTime.length() == 0) {
    dateTime = String(doc["date_time_txt"] | "");
  }
  if (dateTime.length() == 0) {
    return false;
  }

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!parseYmdHms(dateTime, year, month, day, hour, minute, second)) {
    return false;
  }

  long long days = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  long long localUnix = (days * 86400LL) + (hour * 3600LL) + (minute * 60LL) + second;
  long long utcUnix = localUnix - static_cast<long long>(utcOffsetSeconds);
  if (utcUnix <= 0) {
    return false;
  }

  unixTime = static_cast<unsigned long>(utcUnix);
  return true;
}

bool syncFromQuery(const String& query, time_t& baseUtcEpoch, long& utcOffsetSeconds, unsigned long& epochStartMillis) {
  HTTPClient http;
  http.begin(query);
  Serial.println("🌐 Time zone query: " + query);

  int code = http.GET();
  if (code != 200) {
    Serial.println("❌ Time API failed: " + http.errorToString(code));
    http.end();
    return false;
  }

  String payload = http.getString();
  DynamicJsonDocument doc(3072);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("❌ JSON parse error: ");
    Serial.println(err.c_str());
    http.end();
    return false;
  }

  unsigned long currentUnix = jsonToUnsignedLong(doc["date_time_unix"]);
  if (currentUnix == 0UL) currentUnix = jsonToUnsignedLong(doc["current_time_unix"]);
  if (currentUnix == 0UL) currentUnix = jsonToUnsignedLong(doc["time_stamp"]);

  double offsetWithDstHours = jsonToDouble(doc["time_zone"]["offset_with_dst"], NAN);
  if (isnan(offsetWithDstHours)) {
    offsetWithDstHours = jsonToDouble(doc["offset_with_dst"], NAN);
  }
  if (isnan(offsetWithDstHours)) {
    offsetWithDstHours = jsonToDouble(doc["timezone_offset_with_dst"], NAN);
  }
  if (isnan(offsetWithDstHours)) {
    offsetWithDstHours = jsonToDouble(doc["time_zone"]["offset"], 0.0);
  }

  long parsedOffsetSeconds = lround(offsetWithDstHours * 3600.0);
  if (currentUnix == 0UL && !parseUnixFromLocalDateTime(doc, parsedOffsetSeconds, currentUnix)) {
    Serial.println("❌ No parseable timestamp found in timezone response.");
    http.end();
    return false;
  }

  baseUtcEpoch = static_cast<time_t>(currentUnix);
  utcOffsetSeconds = parsedOffsetSeconds;
  epochStartMillis = millis();

  Serial.printf("✅ Time sync complete. unix=%lu offset=%ld timezone=%s\n",
                currentUnix, utcOffsetSeconds, deviceSettings.timezone.c_str());

  http.end();
  return true;
}

}

void TimeCache::init() {
  if (fetchTime()) {
    lastSync = millis();
  } else {
    lastSync = millis();
  }
}

void TimeCache::updateIfNeeded() {
  unsigned long interval = isSynchronized() ? SYNC_INTERVAL : FAILED_RETRY_INTERVAL;
  if (lastSync == 0 || millis() - lastSync > interval) {
    if (fetchTime()) {
      lastSync = millis();
    } else {
      lastSync = millis();
    }
  }
}

bool TimeCache::fetchTime() {
  if (!Firebase.ready()) {
    Serial.println("⚠️ Firebase not ready — skipping time fetch.");
    return false;
  }
  String apiKey = RemoteConfigManager::get("IP_GEO_LOCATION_API_KEY", "");
  if (apiKey == "") {
    Serial.println("❌ API key for timezone not found in secrets.");
    return false;
  }

  String baseQuery = "https://api.ipgeolocation.io/timezone?apiKey=" + apiKey;
  if (deviceSettings.timezone.length() > 0) {
    if (syncFromQuery(baseQuery + "&tz=" + urlEncode(deviceSettings.timezone), baseUtcEpoch, utcOffsetSeconds, epochStartMillis)) {
      return true;
    }
  }

  if (storedLat != 0.0f && storedLon != 0.0f) {
    if (syncFromQuery(baseQuery + "&lat=" + String(storedLat, 4) + "&long=" + String(storedLon, 4),
                      baseUtcEpoch, utcOffsetSeconds, epochStartMillis)) {
      return true;
    }
  }

  Serial.println("❌ Unable to synchronize time from timezone API.");
  return false;
}

tm TimeCache::getCurrentLocalTm() {
  time_t utcNow = baseUtcEpoch + ((millis() - epochStartMillis) / 1000);
  time_t localNow = utcNow + utcOffsetSeconds;
  tm localTm{};
  tm* raw = gmtime(&localNow);
  if (raw != nullptr) {
    localTm = *raw;
  }
  return localTm;
}

String TimeCache::getCurrentTimeString() {
  tm t = getCurrentLocalTm();
  char buf[9];
  sprintf(buf, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  return String(buf);
}

int TimeCache::getHour() {
  return getCurrentLocalTm().tm_hour;
}

int TimeCache::getMinute() {
  return getCurrentLocalTm().tm_min;
}

DisplayTimeParts TimeCache::getDisplayTimeParts() {
  DisplayTimeParts parts;
  int h = getHour();
  int m = getMinute();

  if (timeFormatPreference == 0 || timeFormatPreference == 1) {
    if (timeFormatPreference == 1) {
      parts.suffix = (h >= 12) ? "PM" : "AM";
    }
    h = h % 12;
    if (h == 0) h = 12;
  }

  char buffer[8];
  snprintf(buffer, sizeof(buffer), "%d:%02d", h, m);
  parts.main = String(buffer);
  return parts;
}

String TimeCache::getFormattedTime() {
  DisplayTimeParts parts = getDisplayTimeParts();
  if (parts.suffix.length() == 0) return parts.main;
  return parts.main + " " + parts.suffix;
}

int TimeCache::getWeekdayIndex() {
  return getCurrentLocalTm().tm_wday;
}

int TimeCache::getMinutesSinceMidnight() {
  tm localTm = getCurrentLocalTm();
  return (localTm.tm_hour * 60) + localTm.tm_min;
}

unsigned long TimeCache::getCurrentUnixTime() {
  if (!isSynchronized()) return 0;
  return static_cast<unsigned long>(baseUtcEpoch + ((millis() - epochStartMillis) / 1000));
}

bool TimeCache::isSynchronized() {
  return baseUtcEpoch != 0;
}
