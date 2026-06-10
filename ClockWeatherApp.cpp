#include "ClockWeatherApp.h"

#include <Arduino.h>

#include "DeviceConfig.h"
#include "DisplayHelpers.h"
#include "TimeCache.h"
#include "WeatherCache.h"

extern int brightnessLevel;
extern TimeCache timeCache;
extern bool isUpdating;
extern String units;

namespace {

const char* kWeekdays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

String withBlinkingColon(const String& timeText, bool showColon) {
  if (showColon) return timeText;
  String output = timeText;
  int colonIndex = output.indexOf(':');
  if (colonIndex >= 0) output.setCharAt(colonIndex, ' ');
  return output;
}

int parseSecondFromClockString(const String& clockString) {
  if (clockString.length() < 8) return -1;
  return clockString.substring(6, 8).toInt();
}

uint16_t textWidth(const String& text, uint8_t size) {
  matrix.setTextSize(size);
  matrix.setTextWrap(false);
  int16_t x1, y1;
  uint16_t w, h;
  matrix.getTextBounds(text.c_str(), 0, 0, &x1, &y1, &w, &h);
  return w;
}

String shortCityLabel() {
  String label = weatherData.city;
  if (label.length() == 0) {
    label = deviceSettings.locationLabel;
  }
  label.trim();
  if (label.length() == 0) return "LOCAL";
  int commaIndex = label.indexOf(',');
  if (commaIndex > 0) {
    label = label.substring(0, commaIndex);
  }
  label.trim();
  label.toUpperCase();
  if (label.length() > 8) {
    label = label.substring(0, 8);
  }
  return label;
}

String trimToWidth(String text, int maxWidth) {
  matrix.setTextSize(1);
  matrix.setTextWrap(false);
  while (text.length() > 0) {
    int16_t x1, y1;
    uint16_t w, h;
    matrix.getTextBounds(text.c_str(), 0, 0, &x1, &y1, &w, &h);
    if (static_cast<int>(w) <= maxWidth) break;
    text.remove(text.length() - 1);
  }
  return text;
}

}  // namespace

ClockWeatherApp::ClockWeatherApp() {
  setNeedsRedraw(true);
}

void ClockWeatherApp::init() {
  lastRenderedKey = "";
  setNeedsRedraw(true);
}

void ClockWeatherApp::loop() {
  timeCache.updateIfNeeded();
  int second = parseSecondFromClockString(timeCache.getCurrentTimeString());
  static int lastSecond = -1;
  if (second != lastSecond) {
    lastSecond = second;
    setNeedsRedraw(true);
  }
}

void ClockWeatherApp::redraw(bool force, int xOffset) {
  if (isUpdating || (!force && !getNeedsRedraw())) return;

  DisplayTimeParts timeParts = timeCache.getDisplayTimeParts();
  int second = parseSecondFromClockString(timeCache.getCurrentTimeString());
  bool showColon = (second < 0) ? true : ((second % 2) == 0);
  String timeText = withBlinkingColon(timeParts.main, showColon);
  String tempText = getTemperatureString(units);
  String city = trimToWidth(shortCityLabel(), 34);

  String weekday = "DAY";
  int weekdayIndex = timeCache.getWeekdayIndex();
  if (weekdayIndex >= 0 && weekdayIndex <= 6) {
    weekday = kWeekdays[weekdayIndex];
  }

  String renderKey = weekday + "|" + timeText + "|" + timeParts.suffix + "|" + tempText + "|" + city;
  if (!force && renderKey == lastRenderedKey) return;

  lastRenderedKey = renderKey;
  setNeedsRedraw(false);

  uint16_t dayColor = getScaledColor(120, 210, 255);
  uint16_t timeColor = getScaledColor(255, 255, 255);
  uint16_t suffixColor = getScaledColor(185, 185, 185);
  uint16_t tempColor = getScaledColor(0, 230, 230);
  uint16_t cityColor = getScaledColor(170, 170, 170);

  matrix.fillScreen(0);

  // Top metadata row.
  matrix.setTextSize(1);
  matrix.setTextColor(dayColor);
  matrix.setCursor(1 + xOffset, 1);
  matrix.print(weekday);

  if (timeParts.suffix.length() > 0) {
    matrix.setTextColor(suffixColor);
    uint16_t suffixWidth = textWidth(timeParts.suffix, 1);
    int suffixX = PANEL_WIDTH - static_cast<int>(suffixWidth) - 1 + xOffset;
    if (suffixX < 0) suffixX = 0;
    matrix.setCursor(suffixX, 1);
    matrix.print(timeParts.suffix);
  }

  // Main time row.
  uint8_t timeSize = (textWidth(timeText, 2) <= (PANEL_WIDTH - 2)) ? 2 : 1;
  int timeY = (timeSize == 2) ? 9 : 13;
  showCenteredText(timeText.c_str(), timeY, timeColor, timeSize, xOffset);

  matrix.setTextSize(1);
  matrix.setTextColor(tempColor);
  matrix.setCursor(2 + xOffset, 23);
  matrix.print(tempText);

  matrix.setTextColor(cityColor);
  int16_t x1, y1;
  uint16_t w, h;
  matrix.getTextBounds(city.c_str(), 0, 0, &x1, &y1, &w, &h);
  matrix.setCursor(PANEL_WIDTH - static_cast<int>(w) - 1 + xOffset, 23);
  matrix.print(city);
}

void ClockWeatherApp::setNeedsRedraw(bool flag) {
  needsRedraw = flag;
}

bool ClockWeatherApp::getNeedsRedraw() {
  return needsRedraw;
}
