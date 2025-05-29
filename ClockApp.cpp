#include "ClockApp.h"
#include "DisplayHelpers.h"
#include "TimeCache.h"
#include <Arduino.h>

extern TimeCache timeCache;
extern int brightnessLevel;

void ClockApp::init() {
  lastMinute = -1;  // force initial draw
  lastDisplayedTime = "";
  setNeedsRedraw(true);
}

void ClockApp::loop() {
  timeCache.updateIfNeeded();

  int currentMinute = timeCache.getMinute();
  if (currentMinute != lastMinute) {
    lastMinute = currentMinute;
    setNeedsRedraw(true);
  }
}

void ClockApp::redraw(bool force, int xOffset) {
  if (isUpdating || (!force && !getNeedsRedraw())) return;

  String current = timeCache.getFormattedTime();
  if (!force && current == lastDisplayedTime) return;

  lastDisplayedTime = current;
  setNeedsRedraw(false);

  uint16_t timeColor = getScaledColor(255, 255, 255);

  String timePart, suffix;
  int suffixIndex = current.indexOf(" ");
  if (suffixIndex > 0) {
    timePart = current.substring(0, suffixIndex);
    suffix = current.substring(suffixIndex + 1);
    timePart += "    ";  // Extra spacing
    timePart += suffix;
  } else {
    timePart = current;
  }

  matrix.fillScreen(0);
  showCenteredText(timePart.c_str(), 12, timeColor, 1, xOffset);
}

void ClockApp::setNeedsRedraw(bool flag) {
  needsRedraw = flag;
}

bool ClockApp::getNeedsRedraw() {
  return needsRedraw;
}