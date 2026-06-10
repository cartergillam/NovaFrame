#pragma once

#include <Arduino.h>

struct SportsLogoLayerDef {
  const uint8_t* bitmap;
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

struct SportsLogoDef {
  const char* league;
  const char* team;
  uint8_t width;
  uint8_t height;
  uint8_t layerCount;
  const SportsLogoLayerDef* layers;
};

extern const SportsLogoDef SPORTS_LOGOS[];
extern const size_t SPORTS_LOGO_COUNT;
