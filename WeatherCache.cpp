#include "WeatherCache.h"
#include "DisplayHelpers.h"
#include "secrets.h"
#include <Firebase_ESP_Client.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "DeviceRegistration.h"

extern FirebaseData fbdo;
extern String getSanitizedMac();
extern String units;
extern float storedLat;
extern float storedLon;

WeatherData weatherData;
unsigned long lastWeatherFetchTime = 0;
const unsigned long WEATHER_CACHE_INTERVAL = 600000; // 10 minutes

void updateWeatherCache() {
  unsigned long now = millis();
  if (now - lastWeatherFetchTime < WEATHER_CACHE_INTERVAL && lastWeatherFetchTime != 0) {
    return;
  }

  float lat = storedLat;
  float lon = storedLon;

  if (lat == 0.0 || lon == 0.0) {
    Serial.println("❌ Stored lat/lon are zero. Skipping weather fetch.");
    return;
  }

  Serial.printf("📍 Lat: %.4f | Lon: %.4f\n", lat, lon);
  Serial.print("📏 Units: "); Serial.println(units);

  String query = "http://api.openweathermap.org/data/2.5/weather?lat=" + String(lat, 4)
               + "&lon=" + String(lon, 4)
               + "&appid=" + OPENWEATHER_API_KEY + "&units=" + units;

  Serial.println("🌍 OpenWeather query: " + query);

  HTTPClient http;
  http.begin(query);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    Serial.println("📦 OpenWeather response: " + payload);

    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      Serial.print("❌ JSON parse error: ");
      Serial.println(err.c_str());
      return;
    }

    weatherData.temp      = String((int)round(doc["main"]["temp"].as<float>()));
    weatherData.feelsLike = String((int)round(doc["main"]["feels_like"].as<float>()));
    weatherData.tempHigh  = String((int)round(doc["main"]["temp_max"].as<float>()));
    weatherData.tempLow   = String((int)round(doc["main"]["temp_min"].as<float>()));
    weatherData.city      = doc["name"].as<String>();
    weatherData.icon      = doc["weather"][0]["icon"].as<String>();

    lastWeatherFetchTime = now;
    Serial.println("✅ Weather cache updated");
    Serial.println("🌡️ Temp now: " + weatherData.temp);
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