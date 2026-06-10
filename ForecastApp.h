#pragma once

#include "BaseApp.h"

class ForecastApp : public BaseApp {
public:
  void init() override;
  void loop() override;
  void redraw(bool force = false, int xOffset = 0) override;
  void setNeedsRedraw(bool flag) override;
  bool getNeedsRedraw() override;
  String getAppId() override { return "forecast"; }

private:
  bool needsRedraw = true;
  unsigned long startTime = 0;
};
