#pragma once

#include <Arduino.h>
#include <Adafruit_Protomatter.h>

// 8x8 monochrome bitmaps for basic weather icons
extern const uint8_t bitmap_sun[8];
extern const uint8_t bitmap_cloud[8];
extern const uint8_t bitmap_moon[8];
extern const uint8_t bitmap_rain[8];

// Returns a color for the icon based on the OpenWeatherMap icon code, with brightness scaling
uint16_t getIconColor(const String& iconCode);

// Draws the correct weather icon bitmap at the given x,y based on iconCode
void drawWeatherIcon(const String& iconCode, int x, int y);