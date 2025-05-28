// WeatherCache.h
#pragma once

#include <Arduino.h>

struct WeatherData {
  String temp = "--";
  String feelsLike = "--";
  String tempHigh = "--";
  String tempLow = "--";
  String city = "";
  String icon = "";
};

extern WeatherData weatherData;

String getUnits();
void updateWeatherCache();
String getTemperatureString(const String& unitSystem);