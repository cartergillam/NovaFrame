#pragma once

#include <Adafruit_Protomatter.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include "WeatherCache.h"
#include "TimeCache.h"

#define PANEL_WIDTH 64
#define PANEL_HEIGHT 32
#define NEOPIXEL_PIN 21

extern Adafruit_Protomatter matrix;
extern Adafruit_NeoPixel pixel;
extern int brightnessLevel;

void initializeDisplay();
void showCenteredText(const char* text, int y, uint16_t color, int size = 1, int xOffset = 0);
void scrollText(const char* text, int y, uint16_t color, int delayMs = 40);
void showWifiInfo();
void showConnectingToWiFi();
void showWifiNotSetNotice();
void showJoinInstructions();
void showWelcome();
uint16_t getScaledColor(uint8_t r, uint8_t g, uint8_t b);
void checkBrightnessUpdate();
void checkTimeFormatUpdate();
void checkUnitsUpdate();