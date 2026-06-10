#include "WeatherCache.h"
#include "DisplayHelpers.h"
#include <Firebase_ESP_Client.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "DeviceRegistration.h"
#include "DeviceConfig.h"
#include "RemoteConfigManager.h"
#include "AppManager.h"

extern FirebaseData fbdo;
extern String getSanitizedMac();
extern String units;
extern float storedLat;
extern float storedLon;
extern BaseApp* getActiveApp();
extern AppManager appManager;

WeatherData weatherData;
unsigned long lastWeatherFetchTime = 0;
unsigned long lastWeatherAttemptTime = 0;

namespace {

constexpr unsigned long kWeatherFailureRetryMs = 5UL * 60UL * 1000UL;

unsigned long weatherCacheIntervalMs() {
  return isAppEnabled("clockWeather")
    ? 20UL * 60UL * 1000UL
    : 60UL * 60UL * 1000UL;
}

}

void updateWeatherCache() {
  unsigned long now = millis();
  weatherData.city = deviceSettings.locationLabel;

  if (lastWeatherFetchTime != 0 && now - lastWeatherFetchTime < weatherCacheIntervalMs()) {
    return;
  }
  if (lastWeatherFetchTime == 0 && lastWeatherAttemptTime != 0 && now - lastWeatherAttemptTime < kWeatherFailureRetryMs) {
    return;
  }

  float lat = storedLat;
  float lon = storedLon;

  if (lat == 0.0 || lon == 0.0) {
    Serial.println("❌ Stored lat/lon are zero. Skipping weather fetch.");
    return;
  }

  String apiKey = RemoteConfigManager::get("OPENWEATHER_API_KEY", "");
  if (apiKey == "") {
    Serial.println("❌ One Call API key not found in remote config.");
    return;
  }

  String query = "https://api.openweathermap.org/data/3.0/onecall?lat=" + String(lat, 4)
               + "&lon=" + String(lon, 4)
               + "&exclude=minutely,hourly,alerts"
               + "&units=" + units
               + "&appid=" + apiKey;

  Serial.println("🌍 One Call 3.0 query: " + query);

  HTTPClient http;
  http.begin(query);
  lastWeatherAttemptTime = now;
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    Serial.println("📦 One Call API response received");

    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      Serial.print("❌ JSON parse error: ");
      Serial.println(err.c_str());
      http.end();
      return;
    }

    long timezoneOffset = doc["timezone_offset"] | 0L;

    // Current weather
    weatherData.temp      = String((int)round(doc["current"]["temp"].as<float>()));
    weatherData.feelsLike = String((int)round(doc["current"]["feels_like"].as<float>()));
    weatherData.icon      = doc["current"]["weather"][0]["icon"].as<String>();
    Serial.println("📍 Current icon: " + weatherData.icon);

    // Today
    JsonObject today = doc["daily"][0];
    weatherData.tempHigh = String((int)round(today["temp"]["max"].as<float>()));
    weatherData.tempLow  = String((int)round(today["temp"]["min"].as<float>()));
    weatherData.forecastHigh1 = weatherData.tempHigh;
    weatherData.forecastLow1  = weatherData.tempLow;

    // ✅ Use current.icon instead of daily[0]
    weatherData.icon1 = weatherData.icon;
    Serial.println("🌤️ icon1 set to current icon: " + weatherData.icon1);

    // Tomorrow
    JsonObject tomorrow = doc["daily"][1];
    weatherData.forecastHigh2 = String((int)round(tomorrow["temp"]["max"].as<float>()));
    weatherData.forecastLow2  = String((int)round(tomorrow["temp"]["min"].as<float>()));
    weatherData.icon2 = tomorrow["weather"][0]["icon"].as<String>();
    Serial.println("🔮 icon2 (tomorrow): " + weatherData.icon2);

    // Day names
    time_t todayDT = today["dt"].as<time_t>() + timezoneOffset;
    time_t tomorrowDT = tomorrow["dt"].as<time_t>() + timezoneOffset;
    char buf[4];

    tm* todayTm = gmtime(&todayDT);
    if (todayTm != nullptr) {
      strftime(buf, sizeof(buf), "%a", todayTm);
      weatherData.forecastDay1 = String(buf);
    }

    tm* tomorrowTm = gmtime(&tomorrowDT);
    if (tomorrowTm != nullptr) {
      strftime(buf, sizeof(buf), "%a", tomorrowTm);
      weatherData.forecastDay2 = String(buf);
    }

    lastWeatherFetchTime = now;
    lastWeatherAttemptTime = now;
    Serial.println("✅ Weather cache updated (One Call)");
    Serial.println("🌡️ Temp now: " + weatherData.temp);

    BaseApp* activeApp = getActiveApp();
    if (activeApp != nullptr) {
      activeApp->setNeedsRedraw(true);
    }

  } else {
    Serial.println("❌ OpenWeather HTTP error: " + http.errorToString(code));
  }

  http.end();
}

String getTemperatureString(const String& unitSystem) {
  char degreeSymbol = 247;
  String unit = (unitSystem == "imperial") ? "F" : "C";
  return weatherData.temp + String(degreeSymbol) + unit;
}

String getUnits() {
  return units;
}
