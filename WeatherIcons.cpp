#include "WeatherIcons.h"
#include <Adafruit_Protomatter.h>
#include "DisplayHelpers.h"

extern Adafruit_Protomatter matrix;

const uint8_t bitmap_sun[8] = {
  0b00111100,
  0b01011010,
  0b10111101,
  0b11111111,
  0b11111111,
  0b10111101,
  0b01011010,
  0b00111100
};

const uint8_t bitmap_cloud[8] = {
  0b00000000,
  0b00111000,
  0b01111100,
  0b11111110,
  0b11111110,
  0b01111100,
  0b00111000,
  0b00000000
};

const uint8_t bitmap_moon[8] = {
  0b00011100,
  0b00111100,
  0b01111000,
  0b01110000,
  0b01110000,
  0b01111000,
  0b00111100,
  0b00011100
};

const uint8_t bitmap_rain[8] = {
  0b00111000,
  0b01111100,
  0b11111110,
  0b11111110,
  0b00010000,
  0b00101000,
  0b00010000,
  0b00101000
};

uint16_t getIconColor(const String& iconCode) {
  if (iconCode.startsWith("01") || iconCode.startsWith("02")) {
    return getScaledColor(255, 255, 0);  // Yellow
  } else if (iconCode.startsWith("03") || iconCode.startsWith("04") || iconCode.endsWith("n")) {
    return getScaledColor(255, 255, 255);  // White
  } else if (iconCode.startsWith("09") || iconCode.startsWith("10") || iconCode.startsWith("11") || iconCode.startsWith("13")) {
    return getScaledColor(0, 128, 255);  // Blue
  } else {
    return getScaledColor(192, 192, 192);  // Fallback gray
  }
}

void drawWeatherIcon(const String& iconCode, int x, int y) {
  const uint8_t* iconBitmap = nullptr;

  if (iconCode.startsWith("01") || iconCode.startsWith("02")) {
    iconBitmap = bitmap_sun;
  } else if (iconCode.startsWith("03") || iconCode.startsWith("04")) {
    iconBitmap = bitmap_cloud;
  } else if (iconCode.endsWith("n")) {
    iconBitmap = bitmap_moon;
  } else if (iconCode.startsWith("09") || iconCode.startsWith("10") ||
             iconCode.startsWith("11") || iconCode.startsWith("13")) {
    iconBitmap = bitmap_rain;
  }

  if (iconBitmap) {
    matrix.drawBitmap(x, y, iconBitmap, 8, 8, getIconColor(iconCode));
  } else {
    matrix.setCursor(x, y);
    matrix.setTextColor(getScaledColor(192, 192, 192));
    matrix.print("?");
  }
}