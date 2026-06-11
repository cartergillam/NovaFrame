#include "SportsLogos.h"

#include <Adafruit_Protomatter.h>

#include "DisplayHelpers.h"
#include "SportsLogosGenerated.h"

extern Adafruit_Protomatter matrix;

namespace {

String normalizedLeague(const String& rawLeague) {
  String league = rawLeague;
  league.trim();
  league.toLowerCase();
  return league;
}

String normalizedTeam(const String& rawTeam) {
  String team = rawTeam;
  team.trim();
  team.toUpperCase();
  return team;
}

const SportsLogoDef* findLogo(const String& rawLeague, const String& rawTeam) {
  String league = normalizedLeague(rawLeague);
  String team = normalizedTeam(rawTeam);
  for (size_t i = 0; i < SPORTS_LOGO_COUNT; ++i) {
    const SportsLogoDef& logo = SPORTS_LOGOS[i];
    if (league == logo.league && team == logo.team) {
      return &logo;
    }
  }
  return nullptr;
}

bool bitmapPixelSet(const uint8_t* bitmap, uint8_t width, uint8_t x, uint8_t y) {
  uint16_t byteIndex = static_cast<uint16_t>(y) * ((width + 7) / 8) + (x / 8);
  uint8_t bitMask = 0x80 >> (x % 8);
  return (bitmap[byteIndex] & bitMask) != 0;
}

bool sampledPixelSet(const uint8_t* bitmap, uint8_t sourceWidth, uint8_t sourceHeight,
                     uint8_t outX, uint8_t outY, uint8_t outSize) {
  uint8_t startX = static_cast<uint16_t>(outX) * sourceWidth / outSize;
  uint8_t endX = static_cast<uint16_t>(outX + 1) * sourceWidth / outSize;
  uint8_t startY = static_cast<uint16_t>(outY) * sourceHeight / outSize;
  uint8_t endY = static_cast<uint16_t>(outY + 1) * sourceHeight / outSize;
  if (endX <= startX) endX = startX + 1;
  if (endY <= startY) endY = startY + 1;

  for (uint8_t y = startY; y < endY && y < sourceHeight; ++y) {
    for (uint8_t x = startX; x < endX && x < sourceWidth; ++x) {
      if (bitmapPixelSet(bitmap, sourceWidth, x, y)) return true;
    }
  }
  return false;
}

}  // namespace

bool hasSportsLogo(const String& league, const String& teamId) {
  return findLogo(league, teamId) != nullptr;
}

uint8_t sportsLogoRowSize(const String& rawLeague, const String& rawTeam, uint8_t defaultSize) {
  const SportsLogoDef* logo = findLogo(rawLeague, rawTeam);
  if (logo == nullptr || logo->rowSize == 0) return defaultSize;
  return logo->rowSize < defaultSize ? logo->rowSize : defaultSize;
}

bool sportsLogoDominantColor(const String& league, const String& teamId, uint8_t& r, uint8_t& g, uint8_t& b) {
  const SportsLogoDef* logo = findLogo(league, teamId);
  if (logo == nullptr || logo->layerCount == 0) return false;
  const SportsLogoLayerDef& layer = logo->layers[0];
  r = layer.r;
  g = layer.g;
  b = layer.b;
  return true;
}

bool drawSportsLogoSilhouette(const String& league, const String& teamId, int x, int y, uint8_t size, uint16_t color) {
  const SportsLogoDef* logo = findLogo(league, teamId);
  if (logo == nullptr || size == 0) return false;

  for (uint8_t outY = 0; outY < size; ++outY) {
    for (uint8_t outX = 0; outX < size; ++outX) {
      bool covered = false;
      for (uint8_t layerIndex = 0; layerIndex < logo->layerCount && !covered; ++layerIndex) {
        const SportsLogoLayerDef& layer = logo->layers[layerIndex];
        covered = sampledPixelSet(layer.bitmap, logo->width, logo->height, outX, outY, size);
      }
      if (covered) {
        matrix.drawPixel(x + outX, y + outY, color);
      }
    }
  }
  return true;
}

bool drawSportsLogo(const String& league, const String& teamId, int x, int y, uint8_t size) {
  const SportsLogoDef* logo = findLogo(league, teamId);
  if (logo == nullptr || size == 0) return false;

  for (uint8_t layerIndex = 0; layerIndex < logo->layerCount; ++layerIndex) {
    const SportsLogoLayerDef& layer = logo->layers[layerIndex];
    uint16_t color = getScaledColor(layer.r, layer.g, layer.b);
    for (uint8_t outY = 0; outY < size; ++outY) {
      for (uint8_t outX = 0; outX < size; ++outX) {
        if (sampledPixelSet(layer.bitmap, logo->width, logo->height, outX, outY, size)) {
          matrix.drawPixel(x + outX, y + outY, color);
        }
      }
    }
  }
  return true;
}
