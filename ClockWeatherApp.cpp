#include "ClockWeatherApp.h"
#include "DisplayHelpers.h"
#include "WeatherCache.h"
#include "TimeCache.h"
#include "DeviceRegistration.h"

extern int brightnessLevel;
extern TimeCache timeCache;
extern bool isUpdating;

String lastDisplayedTime = "";

ClockWeatherApp::ClockWeatherApp() {
  setNeedsRedraw(true);
}

void ClockWeatherApp::init() {
  updateWeatherCache();
  timeCache.updateIfNeeded();
  setNeedsRedraw(true);
}

void ClockWeatherApp::loop() {
  updateWeatherCache();
  timeCache.updateIfNeeded();
}

int ClockWeatherApp::getCharWidth(char c) {
  if (c == ':') return 6;
  if (c == '1') return 8;
  return 12;
}

void ClockWeatherApp::redraw(bool force, int xOffset) {
  if (isUpdating || (!force && !getNeedsRedraw())) return;

  String current = timeCache.getFormattedTime();
  if (current != lastDisplayedTime || force || getNeedsRedraw()) {
    lastDisplayedTime = current;
    setNeedsRedraw(false);

    uint16_t timeColor = getScaledColor(255, 255, 255);
    uint16_t tempColor = getScaledColor(0, 255, 255);

    int suffixIndex = current.indexOf(" ");
    String timePart = (suffixIndex > 0) ? current.substring(0, suffixIndex) : current;
    String suffix    = (suffixIndex > 0) ? current.substring(suffixIndex + 1) : "";

    matrix.setTextSize(2);
    matrix.setTextColor(timeColor);

    // --- 24h or 12h (no suffix) ---
    if (timeFormatPreference == 2 || timeFormatPreference == 1) {
      int timeWidth = 0;
      for (char c : timePart) timeWidth += getCharWidth(c);
      int startX = (PANEL_WIDTH - timeWidth) / 2 + xOffset;

      int cursorX = startX;
      for (int i = 0; i < timePart.length(); i++) {
        char c = timePart[i];
        int w = getCharWidth(c);
        int offset = (c == ':' ? -1 : 0);

        matrix.setCursor(cursorX + offset, 4);
        matrix.print(c);
        cursorX += w;
      }
    }

    // --- 12h with suffix ---
    else {
      matrix.setTextSize(1);
      matrix.setTextColor(getScaledColor(180, 180, 180));

      int suffixWidth = (suffix == "PM") ? 12 : 14;
      int suffixRightEdge = PANEL_WIDTH - 1 + xOffset;
      int suffixLeftEdge = suffixRightEdge - suffixWidth + 1;
      int timeRightEdge = suffixLeftEdge - 2;

      int timeWidth = 0;
      for (char c : timePart) timeWidth += getCharWidth(c);
      int timeStartX = timeRightEdge - timeWidth + 1;

      matrix.setTextSize(2);
      matrix.setTextColor(timeColor);

      int cursorX = timeStartX;
      for (int i = 0; i < timePart.length(); i++) {
        char c = timePart[i];
        int w = getCharWidth(c);
        int offset = (c == ':' ? -1 : 0);

        if (i == 0 && timePart.length() >= 3 && timePart[1] == ':') {
          w -= 1;
        }

        matrix.setCursor(cursorX + offset + xOffset, 4);
        matrix.print(c);
        cursorX += w;
      }

      matrix.setTextSize(1);
      matrix.setTextColor(getScaledColor(180, 180, 180));
      matrix.setCursor(suffixLeftEdge + xOffset + 1, 11);
      matrix.print(suffix);
    }

    // Temperature centered below
    String tempStr = getTemperatureString(units);
    showCenteredText(tempStr.c_str(), 20, tempColor, 1, xOffset);
  }
  matrix.show();
}

void ClockWeatherApp::setNeedsRedraw(bool flag) {
  needsRedraw = flag;
}

bool ClockWeatherApp::getNeedsRedraw() {
  return needsRedraw;
}