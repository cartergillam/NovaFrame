#pragma once

#include <Arduino.h>
#include "BaseApp.h"

class ClockWeatherApp : public BaseApp {
public:
    ClockWeatherApp();                  // Constructor
    void init() override;              // Called once on load
    void loop() override;              // Called repeatedly
    void redraw(bool force = false, int xOffset = 0) override;  // Called to draw
    void setNeedsRedraw(bool flag) override;    // Force redraw
    bool getNeedsRedraw() override;             // Check if redraw needed

private:
    bool needsRedraw = true;
    int getCharWidth(char c);
};