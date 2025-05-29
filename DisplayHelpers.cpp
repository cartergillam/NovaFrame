#include "DisplayHelpers.h"
#include "DeviceRegistration.h"
#include "BaseApp.h"
#include "AppManager.h"

uint8_t rgbPins[]  = { 42, 41, 40, 38, 39, 37 };
uint8_t addrPins[] = { 45, 36, 48, 35 };
uint8_t clockPin   = 2;
uint8_t latchPin   = 47;
uint8_t oePin      = 14;

Adafruit_Protomatter matrix(PANEL_WIDTH, 4, 1, rgbPins, 4, addrPins, clockPin, latchPin, oePin, true);
Adafruit_NeoPixel pixel(1, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

extern bool isUpdating;
extern AppManager appManager;  // ✅ updated from currentApp to appManager
extern unsigned long lastWeatherFetchTime;
extern FirebaseData fbdo;
extern int timeFormatPreference;
extern String units;

int brightnessLevel = 10;

void initializeDisplay() {
  pixel.begin();
  pixel.setBrightness(10);
  pixel.setPixelColor(0, pixel.Color(0, 0, 64));
  pixel.show();
  delay(1000);

  ProtomatterStatus status = matrix.begin();
  Serial.printf("Protomatter begin() status: %d\n", status);
  if (status != PROTOMATTER_OK) {
    pixel.setPixelColor(0, pixel.Color(64, 0, 0));
    pixel.show();
    while (1);
  }

  matrix.setTextWrap(false);
}

void showCenteredText(const char* text, int y, uint16_t color, int size, int xOffset) {
  matrix.setTextSize(size);
  matrix.setTextColor(color);
  matrix.setTextWrap(false);

  int16_t x1, y1;
  uint16_t w, h;
  matrix.getTextBounds((char*)text, 0, y, &x1, &y1, &w, &h);

  int16_t x = (PANEL_WIDTH - w) / 2 + xOffset;
  matrix.setCursor(x, y);
  matrix.print(text);
  matrix.show();  // ✅ Ensure it actually appears
}

void scrollText(const char* text, int y, uint16_t color, int delayMs) {
  int charWidth = 6;
  int textWidth = strlen(text) * charWidth;
  if (textWidth <= PANEL_WIDTH) {
    showCenteredText(text, y, color);
    return;
  }
  int spacing = 12;
  int totalWidth = textWidth + spacing;
  int startOffset = (PANEL_WIDTH - charWidth) / 2;
  for (int offset = -startOffset; offset < totalWidth; offset++) {
    matrix.fillRect(0, y, PANEL_WIDTH, 8, 0);
    matrix.setCursor(-offset, y);
    matrix.setTextColor(color);
    matrix.print(text);
    matrix.show();
    delay(delayMs);
  }
}

void showWifiInfo() {
  if (isUpdating) return;

  matrix.fillScreen(0);
  showCenteredText("WiFi OK", 0, matrix.color565(0, 192, 64));
  matrix.show();  // Show the OK message right away
  delay(1000);

  String ssid = WiFi.SSID();
  Serial.println("📶 SSID: " + ssid);

  if (ssid.length() <= 10) {
    // If short, show it centered
    showCenteredText(ssid.c_str(), 10, matrix.color565(192, 192, 192));
    matrix.show();
    delay(2000);  // Let it breathe
  } else {
    // Scroll long SSIDs
    scrollText(ssid.c_str(), 10, matrix.color565(192, 192, 192));
  }
}

void showConnectingToWiFi() {
  matrix.fillScreen(0);
  showCenteredText("Connecting", 6, matrix.color565(255, 255, 0));
  showCenteredText("to WiFi", 16, matrix.color565(255, 255, 0));
  matrix.show();
}

void showWifiNotSetNotice() {
  matrix.fillScreen(0);
  showCenteredText("Wi-Fi", 6, matrix.color565(255, 255, 0));
  showCenteredText("not set", 16, matrix.color565(255, 255, 0));
  matrix.show();
}

void showJoinInstructions() {
  matrix.fillScreen(0);
  matrix.setTextWrap(false);
  showCenteredText("Join", 0, matrix.color565(0, 200, 255));
  showCenteredText("NovaFrame", 10, matrix.color565(255, 255, 255));
  showCenteredText("Setup", 20, matrix.color565(255, 255, 255));
  matrix.show();
}

void showWelcome() {
  matrix.fillScreen(0);
  showCenteredText("NovaFrame", 12, matrix.color565(0, 255, 0));
  matrix.show();
  delay(4000);
}

uint16_t getScaledColor(uint8_t r, uint8_t g, uint8_t b) {
  float scale = max(0.1f, brightnessLevel / 10.0f);
  r = round(r * scale);
  g = round(g * scale);
  b = round(b * scale);
  return matrix.color565(r, g, b);
}

void checkBrightnessUpdate() {
  String path = "/novaFrame/devices/" + getSanitizedMac() + "/settings/brightness";
  if (Firebase.RTDB.getInt(&fbdo, path.c_str())) {
    int newVal = constrain(fbdo.intData(), 1, 10);
    if (newVal != brightnessLevel) {
      brightnessLevel = newVal;
      Serial.printf("Brightness updated to %d\n", brightnessLevel);
    }
  }
}

void checkTimeFormatUpdate() {
  String path = "/novaFrame/devices/" + getSanitizedMac() + "/settings/timeFormat";
  if (Firebase.RTDB.getInt(&fbdo, path.c_str())) {
    int newVal = fbdo.intData();
    if (newVal != timeFormatPreference) {
      timeFormatPreference = newVal;
      Serial.printf("🔄 Time format updated to: %d\n", timeFormatPreference);
      BaseApp* current = appManager.getActiveApp();
      if (current) {
        current->setNeedsRedraw(true);
      }
    }
  }
}

void checkUnitsUpdate() {
  String path = "/novaFrame/devices/" + getSanitizedMac() + "/settings/units";
  if (Firebase.RTDB.getString(&fbdo, path.c_str())) {
    String newVal = fbdo.stringData();
    if (newVal != units) {
      units = newVal;
      Serial.println("🔄 Units updated to: " + units);
      lastWeatherFetchTime = 0;
      BaseApp* current = appManager.getActiveApp();
      if (current) {
        current->setNeedsRedraw(true);
      }
    }
  }
}

void drawCenteredText(const String& text, int x, int y) {
  int16_t x1, y1;
  uint16_t w, h;

  matrix.setTextSize(1);
  matrix.setTextWrap(false);
  matrix.getTextBounds(text, 0, y, &x1, &y1, &w, &h);

  int16_t xPos = x - w / 2;
  matrix.setCursor(xPos, y);
  matrix.setTextColor(getScaledColor(255, 255, 255));
  matrix.print(text);
  matrix.show();
}

void drawSmallText(const String& text, int x, int y) {
  matrix.setTextSize(1);
  matrix.setTextWrap(false);
  matrix.setCursor(x, y);
  matrix.setTextColor(getScaledColor(192, 192, 192));
  matrix.print(text);
  matrix.show();
}

