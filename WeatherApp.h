#pragma once
#include "BaseApp.h"

class WeatherApp : public BaseApp {
public:
  void init() override;
  void loop() override;
  void redraw(bool force = false, int xOffset = 0) override;

  void setNeedsRedraw(bool flag) override;
  bool getNeedsRedraw() override;

private:
  bool needsRedraw = true;
};