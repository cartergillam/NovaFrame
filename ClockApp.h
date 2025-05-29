#pragma once
#include "BaseApp.h"
#include <Arduino.h>

class ClockApp : public BaseApp {
public:
  void init() override;
  void loop() override;
  void redraw(bool force = false, int xOffset = 0) override;

  void setNeedsRedraw(bool flag) override;
  bool getNeedsRedraw() override;
  String getAppId() override { return "clock"; }

private:
  String lastDisplayedTime = "";
  int lastMinute = -1;
  bool isUpdating = false;
  bool needsRedraw = true;
};