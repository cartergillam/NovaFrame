#include "StocksApp.h"

#include <algorithm>
#include <math.h>
#include <vector>

#include "DeviceConfig.h"
#include "DisplayHelpers.h"
#include "TimeCache.h"

extern TimeCache timeCache;

namespace {

const unsigned long kDefaultRotateIntervalMs = 5000;
const unsigned long kMarketStaleSeconds = 90UL * 60UL;
const unsigned long kOffHoursStaleSeconds = 18UL * 60UL * 60UL;
const int kMarketOpenMinutes = (9 * 60) + 30;
const int kMarketCloseMinutes = 16 * 60;

const int kHeaderY = 1;
const int kPriceY = 11;
const int kDeltaY = 22;
const int kLeftPanelX = 1;
const int kPanelWidth = 64;
const int kTrendPanelX = 43;
const int kTrendPanelY = 14;
const int kTrendPanelWidth = 20;
const int kTrendPanelHeight = 17;

String normalizeSymbol(const String& rawSymbol) {
  String symbol = rawSymbol;
  symbol.trim();
  symbol.toUpperCase();
  return symbol;
}

String inferCurrencyCode(const String& symbol) {
  String normalized = normalizeSymbol(symbol);
  if (normalized.endsWith(".TO") ||
      normalized.endsWith(".V") ||
      normalized.endsWith(".NE") ||
      normalized.endsWith(".CNQ") ||
      normalized.endsWith(":CA")) {
    return "CAD";
  }
  return "USD";
}

int textWidth(const String& text) {
  matrix.setTextSize(1);
  matrix.setTextWrap(false);
  int16_t x1, y1;
  uint16_t w, h;
  matrix.getTextBounds(text.c_str(), 0, 0, &x1, &y1, &w, &h);
  return static_cast<int>(w);
}

String sanitizeNumericTail(String text) {
  if (text.length() == 0) return text;

  while (text.endsWith(".")) {
    text.remove(text.length() - 1);
  }

  int pctIndex = text.indexOf('%');
  if (pctIndex > 0 && text[pctIndex - 1] == '.') {
    text.remove(pctIndex - 1, 1);
  }

  return text;
}

String fitToWidthSafe(String text, int maxWidth, int minChars) {
  while (text.length() > minChars && textWidth(text) > maxWidth) {
    text.remove(text.length() - 1);
  }
  return sanitizeNumericTail(text);
}

String fitTextToWidth(String text, int maxWidth, int minChars) {
  while (text.length() > minChars && textWidth(text) > maxWidth) {
    text.remove(text.length() - 1);
  }
  return text;
}

String formatPriceForWidth(double price, int maxWidth) {
  for (int decimals = 2; decimals >= 1; --decimals) {
    String candidate = "$" + String(price, static_cast<unsigned int>(decimals));
    candidate = sanitizeNumericTail(candidate);
    if (textWidth(candidate) <= maxWidth) {
      return candidate;
    }
  }
  return fitToWidthSafe("$" + String(price, 1), maxWidth, 3);
}

String formatDollarChangeForWidth(double change, int maxWidth) {
  double magnitude = fabs(change);
  String sign = change < 0 ? "-" : "+";
  for (int decimals = 2; decimals >= 0; --decimals) {
    String candidate = sign + "$" + String(magnitude, static_cast<unsigned int>(decimals));
    candidate = sanitizeNumericTail(candidate);
    if (textWidth(candidate) <= maxWidth) {
      return candidate;
    }
  }
  return fitToWidthSafe(sign + "$" + String(magnitude, 0), maxWidth, 3);
}

String formatPercentChangeForWidth(double changePct, int maxWidth) {
  double magnitude = fabs(changePct);
  String sign = changePct < 0 ? "-" : "+";
  for (int decimals = 2; decimals >= 0; --decimals) {
    String candidate = sign + String(magnitude, static_cast<unsigned int>(decimals)) + "%";
    candidate = sanitizeNumericTail(candidate);
    if (textWidth(candidate) <= maxWidth) {
      return candidate;
    }
  }
  return fitToWidthSafe(sign + String(magnitude, 0) + "%", maxWidth, 3);
}

String resolveCurrencyCode(const StockRuntimeEntry& entry, const String& symbol) {
  String code = entry.currency;
  code.trim();
  code.toUpperCase();
  if (code.length() == 0) {
    return inferCurrencyCode(symbol);
  }
  return code;
}

bool isMarketHoursNow() {
  if (!timeCache.isSynchronized()) return false;
  int weekday = timeCache.getWeekdayIndex();
  if (weekday == 0 || weekday == 6) return false;
  int minutes = timeCache.getMinutesSinceMidnight();
  return minutes >= kMarketOpenMinutes && minutes < kMarketCloseMinutes;
}

bool isStaleQuote(const StockRuntimeEntry& entry) {
  if (entry.stale) return true;
  if (!entry.hasQuote || entry.asOf == 0 || !timeCache.isSynchronized()) return false;

  unsigned long nowUnix = timeCache.getCurrentUnixTime();
  if (nowUnix <= entry.asOf) return false;
  unsigned long ageSeconds = nowUnix - entry.asOf;
  unsigned long threshold = isMarketHoursNow() ? kMarketStaleSeconds : kOffHoursStaleSeconds;
  return ageSeconds > threshold;
}

void drawTrendline(const std::vector<float>& values, int x, int y, int width, int height, uint16_t color) {
  if (values.size() < 2) return;

  float minValue = values[0];
  float maxValue = values[0];
  for (float value : values) {
    minValue = min(minValue, value);
    maxValue = max(maxValue, value);
  }

  float range = max(0.01f, maxValue - minValue);
  int stepCount = static_cast<int>(values.size()) - 1;
  for (int i = 0; i < stepCount; ++i) {
    float startValue = values[i];
    float endValue = values[i + 1];
    int startX = x + ((i * width) / stepCount);
    int endX = x + (((i + 1) * width) / stepCount);
    int startY = y + height - 1 - static_cast<int>(((startValue - minValue) / range) * (height - 1));
    int endY = y + height - 1 - static_cast<int>(((endValue - minValue) / range) * (height - 1));
    matrix.drawLine(startX, startY, endX, endY, color);
  }
}

std::vector<String> buildDisplaySymbols(const AppConfig* config) {
  std::vector<String> symbols;
  if (config != nullptr) {
    for (const String& rawSymbol : config->symbols) {
      String symbol = normalizeSymbol(rawSymbol);
      if (symbol.length() == 0) continue;
      if (std::find(symbols.begin(), symbols.end(), symbol) == symbols.end()) {
        symbols.push_back(symbol);
      }
    }
  }

  if (!symbols.empty()) return symbols;

  const DeviceStocksRuntime& runtime = getDeviceStocksRuntime();
  for (const String& rawSymbol : runtime.symbols) {
    String symbol = normalizeSymbol(rawSymbol);
    if (symbol.length() == 0) continue;
    if (std::find(symbols.begin(), symbols.end(), symbol) == symbols.end()) {
      symbols.push_back(symbol);
    }
  }
  return symbols;
}

}  // namespace

void StocksApp::init() {
  currentSymbolIndex = 0;
  lastRotateAt = millis();
  setNeedsRedraw(true);
}

void StocksApp::loop() {
  const AppConfig* config = getAppConfig(getAppId());
  std::vector<String> symbols = buildDisplaySymbols(config);
  if (symbols.empty()) return;
  unsigned long rotateIntervalMs = kDefaultRotateIntervalMs;
  if (config != nullptr && config->symbolDurationMs > 0) {
    rotateIntervalMs = constrain(config->symbolDurationMs, 1500UL, 60000UL);
  }

  if (currentSymbolIndex >= static_cast<int>(symbols.size())) {
    currentSymbolIndex = 0;
    setNeedsRedraw(true);
  }

  if (symbols.size() > 1 && millis() - lastRotateAt >= rotateIntervalMs) {
    currentSymbolIndex = (currentSymbolIndex + 1) % symbols.size();
    lastRotateAt = millis();
    setNeedsRedraw(true);
  }
}

void StocksApp::redraw(bool force, int xOffset) {
  if (!force && !getNeedsRedraw()) return;

  matrix.fillScreen(0);

  const AppConfig* config = getAppConfig(getAppId());
  std::vector<String> symbols = buildDisplaySymbols(config);
  if (symbols.empty()) {
    drawSmallText("Stocks", 2 + xOffset, 8);
    drawSmallText("Set symbols", 2 + xOffset, 20);
    setNeedsRedraw(false);
    return;
  }

  if (currentSymbolIndex >= static_cast<int>(symbols.size())) {
    currentSymbolIndex = 0;
  }
  String symbol = symbols[currentSymbolIndex];
  matrix.setTextSize(1);
  matrix.setTextWrap(false);

  StockRuntimeEntry entry;
  bool hasEntry = getStockRuntimeEntry(symbol, entry);
  bool hasQuote = hasEntry && entry.hasQuote;
  bool hasTrendline = hasEntry && entry.hasTrendline;
  bool stale = hasEntry && isStaleQuote(entry);

  if (stale) {
    uint16_t staleColor = getScaledColor(255, 180, 0);
    matrix.drawPixel(61 + xOffset, 2, staleColor);
    matrix.drawPixel(60 + xOffset, 2, staleColor);
    matrix.drawPixel(61 + xOffset, 3, staleColor);
    matrix.drawPixel(60 + xOffset, 3, staleColor);
  }

  if (!hasQuote && !hasTrendline) {
    drawSmallText("No cached", 2 + xOffset, 14);
    drawSmallText("data yet", 2 + xOffset, 24);
    setNeedsRedraw(false);
    return;
  }

  uint16_t upColor = getScaledColor(0, 255, 0);
  uint16_t downColor = getScaledColor(255, 0, 0);
  uint16_t neutralColor = getScaledColor(255, 255, 255);
  uint16_t mutedColor = getScaledColor(192, 192, 192);

  String currencyCode = hasQuote ? resolveCurrencyCode(entry, symbol) : inferCurrencyCode(symbol);
  String currencyLabel = currencyCode == "CAD" ? "CAD" : "USD";
  String symbolText = fitTextToWidth(symbol, 38, 1);
  int currencyWidth = textWidth(currencyLabel);
  int maxSymbolWidth = kPanelWidth - currencyWidth - 5;
  symbolText = fitTextToWidth(symbolText, maxSymbolWidth, 1);

  drawSmallText(symbolText, kLeftPanelX + xOffset, kHeaderY);
  matrix.setTextColor(mutedColor);
  matrix.setCursor((kPanelWidth - currencyWidth - 1) + xOffset, kHeaderY);
  matrix.print(currencyLabel);

  if (hasQuote) {
    String mode = "dollar";
    if (config != nullptr) {
      mode = config->valueMode;
      mode.toLowerCase();
      if (mode != "percent") mode = "dollar";
    }

    String priceText = formatPriceForWidth(entry.price, 42);
    String deltaText = mode == "percent"
      ? formatPercentChangeForWidth(entry.changePct, 42)
      : formatDollarChangeForWidth(entry.change, 42);

    matrix.setTextSize(1);
    matrix.setTextColor(neutralColor);
    matrix.setCursor(kLeftPanelX + xOffset, kPriceY);
    matrix.print(priceText);

    double deltaSign = mode == "percent" ? entry.changePct : entry.change;
    uint16_t moveColor = deltaSign < 0.0 ? downColor : upColor;
    matrix.setTextColor(moveColor);
    matrix.setCursor(kLeftPanelX + xOffset, kDeltaY);
    matrix.print(deltaText);
  } else {
    drawSmallText("Quote", kLeftPanelX + xOffset, 14);
    drawSmallText("pend", kLeftPanelX + xOffset, 24);
  }

  if (hasTrendline) {
    uint16_t trendColor = hasQuote ? (entry.change < 0.0 ? downColor : upColor) : neutralColor;
    drawTrendline(entry.trendline, kTrendPanelX + xOffset, kTrendPanelY, kTrendPanelWidth, kTrendPanelHeight, trendColor);
  } else {
    drawSmallText("Trend", kTrendPanelX + xOffset, 18);
    drawSmallText("pend", kTrendPanelX + xOffset, 26);
  }

  setNeedsRedraw(false);
}

void StocksApp::setNeedsRedraw(bool flag) {
  needsRedraw = flag;
}

bool StocksApp::getNeedsRedraw() {
  return needsRedraw;
}
