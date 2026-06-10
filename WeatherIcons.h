#pragma once

#include <Arduino.h>
#include <Adafruit_Protomatter.h>

// Masks
extern const uint8_t bitmap_cloud[];
extern const uint8_t bitmap_sun[72];
extern const uint8_t bitmap_partial_sun[];
extern const uint8_t bitmap_few_clouds[];
extern const uint8_t bitmap_broken_clouds[];
extern const uint8_t bitmap_broken_clouds_black[];
extern const uint8_t bitmap_rain[];
extern const uint8_t bitmap_lightning[];
extern const uint8_t bitmap_snow[];
extern const uint8_t bitmap_mist[];

// Returns a color for the icon based on the OpenWeatherMap icon code, with brightness scaling
uint16_t getIconColor(const String& iconCode);

// Draws the correct weather icon bitmap at the given x,y based on iconCode
void drawWeatherIcon(const String& iconCode, int x, int y);
