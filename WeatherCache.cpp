#include "WeatherCache.h"
#include "DisplayHelpers.h"
#include <Firebase_ESP_Client.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "DeviceRegistration.h"
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
const unsigned long WEATHER_CACHE_INTERVAL = 600000; // 10 minutes

void updateWeatherCache() {
  unsigned long now = millis();
  const unsigned long WEATHER_CACHE_INTERVAL = 30UL * 60UL * 1000UL; // 30 minutes

  if (now - lastWeatherFetchTime < WEATHER_CACHE_INTERVAL && lastWeatherFetchTime != 0) {
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
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    Serial.println("📦 One Call API response received");

    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      Serial.print("❌ JSON parse error: ");
      Serial.println(err.c_str());
      return;
    }

    // Current weather
    weatherData.temp      = String((int)round(doc["current"]["temp"].as<float>()));
    weatherData.feelsLike = String((int)round(doc["current"]["feels_like"].as<float>()));
    weatherData.icon      = doc["current"]["weather"][0]["icon"].as<String>();

    // Today
    JsonObject today = doc["daily"][0];
    weatherData.tempHigh = String((int)round(today["temp"]["max"].as<float>()));
    weatherData.tempLow  = String((int)round(today["temp"]["min"].as<float>()));
    weatherData.forecastHigh1 = weatherData.tempHigh;
    weatherData.forecastLow1  = weatherData.tempLow;
    weatherData.icon1 = today["weather"][0]["icon"].as<String>();

    // Tomorrow
    JsonObject tomorrow = doc["daily"][1];
    weatherData.forecastHigh2 = String((int)round(tomorrow["temp"]["max"].as<float>()));
    weatherData.forecastLow2  = String((int)round(tomorrow["temp"]["min"].as<float>()));
    weatherData.icon2 = tomorrow["weather"][0]["icon"].as<String>();

    // Day names
    time_t todayDT = today["dt"].as<time_t>();
    time_t tomorrowDT = tomorrow["dt"].as<time_t>();
    char buf[4];

    strftime(buf, sizeof(buf), "%a", localtime(&todayDT));
    weatherData.forecastDay1 = String(buf);

    strftime(buf, sizeof(buf), "%a", localtime(&tomorrowDT));
    weatherData.forecastDay2 = String(buf);

    lastWeatherFetchTime = now;
    Serial.println("✅ Weather cache updated (One Call)");
    Serial.println("🌡️ Temp now: " + weatherData.temp);
    // 🔁 Force ForecastApp to redraw if it's currently showing
    BaseApp* activeApp = getActiveApp();
    if (activeApp != nullptr && activeApp->getAppId() == "forecast") {
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