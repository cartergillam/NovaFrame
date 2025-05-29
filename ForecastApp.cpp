// ForecastApp.cpp — Forecast with weather icons
#include "ForecastApp.h"
#include "WeatherCache.h"
#include "DisplayHelpers.h"
#include "WeatherIcons.h"              // ✅ Bitmap icon rendering
#include "Adafruit_Protomatter.h"

extern Adafruit_Protomatter matrix;

void ForecastApp::init() {
  scrollX = 0;
  startTime = millis();
  setNeedsRedraw(true);  // <== ✅ This is the KEY line
  matrix.fillScreen(0);

  Serial.println("📟 ForecastApp initialized");
  Serial.println("Day1: " + weatherData.forecastDay1);
  Serial.println("Icon1: " + weatherData.icon1);
  Serial.println("High1: " + weatherData.forecastHigh1);
  Serial.println("Low1: " + weatherData.forecastLow1);
}

void ForecastApp::loop() {
  if (getNeedsRedraw()) {
    redraw(true);
    setNeedsRedraw(false);
  }
}

void ForecastApp::redraw(bool force, int xOffset) {
  if (!force && !needsRedraw) return;

  matrix.fillScreen(0);

  char degree = 247;
  String high1 = weatherData.forecastHigh1 + degree;
  String low1  = weatherData.forecastLow1 + degree;
  String high2 = weatherData.forecastHigh2 + degree;
  String low2  = weatherData.forecastLow2 + degree;

  uint16_t gray = getScaledColor(192, 192, 192);
  uint16_t blue = getScaledColor(0, 128, 255);

  // --- Day 1 ---
  drawWeatherIcon(weatherData.icon1, scrollX + 2, 2);              // Bitmap icon
  drawSmallText(weatherData.forecastDay1, scrollX + 2, 24);        // Day name

  matrix.setTextColor(gray);
  matrix.setCursor(scrollX + 2, 14);
  matrix.print(high1);
  matrix.setCursor(scrollX + 2, 20);
  matrix.print(low1);

  // --- Divider ---
  for (int y = 0; y < 32; y++) {
    matrix.drawPixel(scrollX + 31, y, blue);
  }

  // --- Day 2 ---
  drawSmallText(weatherData.forecastDay2, scrollX + 36, 0);

  matrix.setTextColor(gray);
  matrix.setCursor(scrollX + 36, 12);
  matrix.print(high2);
  matrix.setCursor(scrollX + 36, 20);
  matrix.print(low2);

  drawWeatherIcon(weatherData.icon2, scrollX + 36, 26);            // Bitmap icon

  matrix.show();
}

void ForecastApp::setNeedsRedraw(bool flag) {
  needsRedraw = flag;
}

bool ForecastApp::getNeedsRedraw() {
  return needsRedraw;
}