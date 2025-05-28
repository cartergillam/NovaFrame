#include "WeatherApp.h"
#include "DisplayHelpers.h"
#include "WeatherCache.h"
#include "DeviceRegistration.h"

void WeatherApp::init() {
  setNeedsRedraw(true);  // Trigger initial draw
}

void WeatherApp::loop() {
  // No local animation needed — AppManager handles it
}

void WeatherApp::redraw(bool force, int xOffset) {
  if (!force && !getNeedsRedraw()) return;

  // Weather data from cache
  String temp = weatherData.temp;
  String city = weatherData.city;

  matrix.setTextSize(2);
  matrix.setTextColor(getScaledColor(255, 255, 255));
  matrix.setCursor(0 + xOffset, 0);
  matrix.print("*");  // Icon placeholder

  char degree = 247;
  String tempStr = temp + String(degree) + (units == "imperial" ? "F" : "C");
  matrix.setCursor(18 + xOffset, 6);
  matrix.setTextColor(getScaledColor(0, 255, 255));
  matrix.print(tempStr);

  matrix.setTextSize(1);
  matrix.setCursor(0 + xOffset, 24);
  matrix.setTextColor(getScaledColor(255, 255, 255));
  matrix.print(city);

  matrix.setTextSize(2);  // Reset
  matrix.show();
  setNeedsRedraw(false);
}

void WeatherApp::setNeedsRedraw(bool flag) {
  needsRedraw = flag;
}

bool WeatherApp::getNeedsRedraw() {
  return needsRedraw;
}