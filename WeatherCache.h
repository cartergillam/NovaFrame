// WeatherCache.h
#pragma once

#include <Arduino.h>

struct WeatherData {
  // Current conditions
  String temp = "--";
  String feelsLike = "--";
  String tempHigh = "--";
  String tempLow = "--";
  String city = "";
  String icon = "";

  // Forecast for Today
  String forecastDay1 = "";
  String forecastHigh1 = "--";
  String forecastLow1 = "--";
  String icon1 = "";

  // Forecast for Tomorrow
  String forecastDay2 = "";
  String forecastHigh2 = "--";
  String forecastLow2 = "--";
  String icon2 = "";
};

extern WeatherData weatherData;

String getUnits();
void updateWeatherCache();
String getTemperatureString(const String& unitSystem);