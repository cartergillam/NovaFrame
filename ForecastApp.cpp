// ForecastApp.cpp — Forecast with weather icons
#include "ForecastApp.h"

#include "Adafruit_Protomatter.h"
#include "DisplayHelpers.h"
#include "WeatherCache.h"
#include "WeatherIcons.h"

enum ForecastAnimPhase {
  WAIT_IN,
  SLIDE_IN,
  HOLD_IN,
  SLIDE_OUT,
  HOLD_OUT
};

extern Adafruit_Protomatter matrix;

namespace {

unsigned long animStartTime = 0;
unsigned long lastFrameTime = 0;
const int FRAME_INTERVAL = 33;  // ~30 FPS

ForecastAnimPhase animPhase = WAIT_IN;
int day1X = 2;
int day1Y = 24;
int day1TextX = 24;
int day1TextY = -20;
float scrollX = 0;

// Cached colors (only update during HOLD)
uint16_t white = 0;
uint16_t dividerBlue = 0;

float easeInOut(float t) {
  t = constrain(t, 0.0, 1.0);
  return t * t * (3 - 2 * t);
}

void updateCachedColors() {
  white = getScaledColor(255, 255, 255);
  dividerBlue = getScaledColor(0, 128 * 0.3, 255 * 0.3);
}

}  // namespace

void ForecastApp::init() {
  scrollX = 0;
  startTime = millis();
  setNeedsRedraw(true);
  matrix.fillScreen(0);
  animStartTime = millis();
  lastFrameTime = millis();
  animPhase = WAIT_IN;
  day1TextX = 24;
  day1TextY = -20;
  updateCachedColors();

  Serial.println("📟 ForecastApp initialized");
  Serial.println("Day1: " + weatherData.forecastDay1);
  Serial.println("Icon1: " + weatherData.icon1);
  Serial.println("Day2: " + weatherData.forecastDay2);
  Serial.println("Icon2: " + weatherData.icon2);
  Serial.println("High1: " + weatherData.forecastHigh1);
  Serial.println("Low1: " + weatherData.forecastLow1);
}

void ForecastApp::loop() {
  unsigned long now = millis();
  if (now - lastFrameTime < FRAME_INTERVAL) return;
  lastFrameTime = now;

  float progress = 0.0f;
  float eased = 0.0f;

  switch (animPhase) {
    case WAIT_IN:
      if (now - animStartTime > 5000) {
        animPhase = SLIDE_IN;
        animStartTime = now;
      }
      break;

    case SLIDE_IN:
      progress = float(now - animStartTime) / 1000.0f;
      eased = easeInOut(progress);
      scrollX = -21.0f * eased;
      day1TextX = static_cast<int>(round(24.0f - (24.0f - 3.0f) * eased));
      day1TextY = static_cast<int>(round(-20.0f + (1.0f + 20.0f) * eased));
      setNeedsRedraw(true);

      if (progress >= 1.0f) {
        animPhase = HOLD_IN;
        animStartTime = now;
        updateCachedColors();
        setNeedsRedraw(true);
      }
      break;

    case HOLD_IN:
      if (now - animStartTime > 5000) {
        animPhase = SLIDE_OUT;
        animStartTime = now;
      }
      break;

    case SLIDE_OUT:
      progress = float(now - animStartTime) / 1000.0f;
      eased = easeInOut(progress);
      scrollX = -21.0f * (1.0f - eased);
      day1TextX = static_cast<int>(round(3.0f + (day1X - 3.0f) * eased));
      day1TextY = static_cast<int>(round(1.0f + (day1Y - 1.0f) * eased));
      setNeedsRedraw(true);

      if (progress >= 1.0f) {
        animPhase = HOLD_OUT;
        animStartTime = now;
        updateCachedColors();
        setNeedsRedraw(true);
      }
      break;

    case HOLD_OUT:
      if (now - animStartTime > 7000) {
        animPhase = SLIDE_IN;
        animStartTime = now;
      }
      break;
  }
}

void ForecastApp::redraw(bool force, int xOffset) {
  if (!force && !needsRedraw) return;
  (void)xOffset;

  matrix.fillScreen(0);

  char degree = 247;
  String high1 = weatherData.forecastHigh1 + degree;
  String low1 = weatherData.forecastLow1 + degree;
  String high2 = weatherData.forecastHigh2 + degree;
  String low2 = weatherData.forecastLow2 + degree;

  matrix.setTextWrap(false);
  matrix.setTextColor(white);

  int drawScrollX = static_cast<int>(round(scrollX));

  if (animPhase == SLIDE_IN || animPhase == SLIDE_OUT) {
    drawSmallText(weatherData.forecastDay1, day1TextX, day1TextY);
  } else if (animPhase == HOLD_IN) {
    drawSmallText(weatherData.forecastDay1, 3, 1);
  } else {
    drawSmallText(weatherData.forecastDay1, day1X + drawScrollX, day1Y);
  }

  float animProgress = float(millis() - animStartTime) / 1000.0f;
  float animEased = easeInOut(animProgress);
  int iconOffset = 0;

  if (animPhase == SLIDE_IN) {
    iconOffset = static_cast<int>(round(-2.0f * animEased));
  } else if (animPhase == HOLD_IN) {
    iconOffset = -2;
  } else if (animPhase == SLIDE_OUT) {
    iconOffset = static_cast<int>(round(-2.0f * (1.0f - animEased)));
  }

  drawWeatherIcon(weatherData.icon1, drawScrollX - 9 + iconOffset, -12);

  int16_t x1, y1;
  uint16_t w, h;
  matrix.getTextBounds(high1.c_str(), 0, 0, &x1, &y1, &w, &h);
  matrix.setCursor(drawScrollX + 24, 14);
  matrix.print(high1);

  matrix.getTextBounds(low1.c_str(), 0, 0, &x1, &y1, &w, &h);
  matrix.setCursor(drawScrollX + 24, 24);
  matrix.print(low1);

  for (int y = 0; y < 32; y++) {
    matrix.drawPixel(drawScrollX + 42, y, dividerBlue);
  }

  drawSmallText(weatherData.forecastDay2, drawScrollX + 44, 1);

  int rightEdgeRight = 63;
  matrix.getTextBounds(high2.c_str(), 0, 0, &x1, &y1, &w, &h);
  matrix.setCursor(rightEdgeRight - static_cast<int>(w) - 1 + drawScrollX, 14);
  matrix.print(high2);

  matrix.getTextBounds(low2.c_str(), 0, 0, &x1, &y1, &w, &h);
  matrix.setCursor(rightEdgeRight - static_cast<int>(w) - 1 + drawScrollX, 24);
  matrix.print(low2);

  // Secondary icon needs a different anchor than the primary because
  // some icon families are 32-40px wide and would clip off-screen at +54.
  int secondaryIconX = drawScrollX + 56;
  int secondaryIconY = 2;
  if (weatherData.icon2.startsWith("03")) {
    secondaryIconX = drawScrollX + 50;
    secondaryIconY = -2;
  } else if (weatherData.icon2.startsWith("01")) {
    secondaryIconX = drawScrollX + 58;
    secondaryIconY = 3;
  }
  if (animPhase == HOLD_IN) {
    // Day-2 held position: nudge icon up/left only in this state.
    secondaryIconX -= 8;  // existing -2 plus requested -6
    secondaryIconY -= 5;
  }
  secondaryIconX += 5;
  drawWeatherIcon(weatherData.icon2, secondaryIconX, secondaryIconY);
  needsRedraw = false;
}

void ForecastApp::setNeedsRedraw(bool flag) {
  needsRedraw = flag;
}

bool ForecastApp::getNeedsRedraw() {
  return needsRedraw;
}
