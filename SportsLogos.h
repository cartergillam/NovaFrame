#pragma once

#include <Arduino.h>

bool drawSportsLogo(const String& league, const String& teamId, int x, int y, uint8_t size = 16);
bool hasSportsLogo(const String& league, const String& teamId);
uint8_t sportsLogoRowSize(const String& league, const String& teamId, uint8_t defaultSize = 12);
bool sportsLogoDominantColor(const String& league, const String& teamId, uint8_t& r, uint8_t& g, uint8_t& b);
bool drawSportsLogoSilhouette(const String& league, const String& teamId, int x, int y, uint8_t size, uint16_t color);
