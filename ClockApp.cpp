#include "ClockApp.h"

#include <Arduino.h>

#include "DisplayHelpers.h"
#include "TimeCache.h"

extern TimeCache timeCache;
extern int brightnessLevel;
extern bool isUpdating;

namespace {

const char* kWeekdays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

String withBlinkingColon(const String& timeText, bool showColon) {
  if (showColon) return timeText;
  String output = timeText;
  int colonIndex = output.indexOf(':');
  if (colonIndex >= 0) {
    output.setCharAt(colonIndex, ' ');
  }
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

}  // namespace

void ClockApp::init() {
  lastSecond = -1;
  lastRenderedKey = "";
  setNeedsRedraw(true);
}

void ClockApp::loop() {
  timeCache.updateIfNeeded();

  int second = parseSecondFromClockString(timeCache.getCurrentTimeString());
  if (second != lastSecond) {
    lastSecond = second;
    setNeedsRedraw(true);
  }
}

void ClockApp::redraw(bool force, int xOffset) {
  if (isUpdating || (!force && !getNeedsRedraw())) return;

  int second = parseSecondFromClockString(timeCache.getCurrentTimeString());
  bool showColon = (second < 0) ? true : ((second % 2) == 0);
  DisplayTimeParts parts = timeCache.getDisplayTimeParts();
  String mainText = withBlinkingColon(parts.main, showColon);
  String weekday = "DAY";
  int weekdayIndex = timeCache.getWeekdayIndex();
  if (weekdayIndex >= 0 && weekdayIndex <= 6) {
    weekday = kWeekdays[weekdayIndex];
  }
  String renderKey = weekday + "|" + mainText + "|" + parts.suffix;
  if (!force && renderKey == lastRenderedKey) return;

  lastRenderedKey = renderKey;
  setNeedsRedraw(false);

  uint16_t dayColor = getScaledColor(120, 210, 255);
  uint16_t timeColor = getScaledColor(255, 255, 255);
  uint16_t suffixColor = getScaledColor(180, 180, 180);

  matrix.fillScreen(0);

  // Top metadata row: weekday (left) + AM/PM (right).
  matrix.setTextSize(1);
  matrix.setTextColor(dayColor);
  matrix.setTextWrap(false);
  matrix.setCursor(1 + xOffset, 1);
  matrix.print(weekday);

  if (parts.suffix.length() > 0) {
    matrix.setTextColor(suffixColor);
    uint16_t suffixWidth = textWidth(parts.suffix, 1);
    int suffixX = PANEL_WIDTH - static_cast<int>(suffixWidth) - 1 + xOffset;
    if (suffixX < 0) suffixX = 0;
    matrix.setCursor(suffixX, 1);
    matrix.print(parts.suffix);
  }

  // Main time row: prefer large digits, fallback if width-constrained.
  uint8_t timeSize = (textWidth(mainText, 2) <= (PANEL_WIDTH - 2)) ? 2 : 1;
  int timeY = (timeSize == 2) ? 12 : 15;
  showCenteredText(mainText.c_str(), timeY, timeColor, timeSize, xOffset);
}

void ClockApp::setNeedsRedraw(bool flag) {
  needsRedraw = flag;
}

bool ClockApp::getNeedsRedraw() {
  return needsRedraw;
}
