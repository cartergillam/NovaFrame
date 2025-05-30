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

  uint16_t white = getScaledColor(255, 255, 255);
  uint16_t dividerBlue = getScaledColor(0, (uint8_t)(128 * 0.3), (uint8_t)(255 * 0.3));

  // ───── LEFT SIDE ─────
  drawWeatherIcon(weatherData.icon1, -4, -8); // 32x32, left-aligned
  drawSmallText(weatherData.forecastDay1, 2, 24); // bottom-left corner

  matrix.setTextColor(white);

  int16_t x1, y1;
  uint16_t w, h;
  int rightEdgeLeft = 41;  // 1px left of divider

  matrix.getTextBounds(high1.c_str(), 0, 0, &x1, &y1, &w, &h);
  matrix.setCursor(rightEdgeLeft - w, 14);
  matrix.print(high1);

  matrix.getTextBounds(low1.c_str(), 0, 0, &x1, &y1, &w, &h);
  matrix.setCursor(rightEdgeLeft - w, 24);
  matrix.print(low1);

  // ───── DIVIDER ─────
  for (int y = 0; y < 32; y++) {
    matrix.drawPixel(42, y, dividerBlue);
  }

  // ───── RIGHT SIDE ─────
  drawSmallText(weatherData.forecastDay2, 45, 1); // top-right

  int rightEdgeRight = 63; // max pixel on 64px width

  matrix.getTextBounds(high2.c_str(), 0, 0, &x1, &y1, &w, &h);
  matrix.setCursor(rightEdgeRight - w, 14);
  matrix.print(high2);

  matrix.getTextBounds(low2.c_str(), 0, 0, &x1, &y1, &w, &h);
  matrix.setCursor(rightEdgeRight - w, 24);
  matrix.print(low2);

  matrix.show();
}

void ForecastApp::setNeedsRedraw(bool flag) {
  needsRedraw = flag;
}

bool ForecastApp::getNeedsRedraw() {
  return needsRedraw;
}