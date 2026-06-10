#pragma once

#include "BaseApp.h"

class StocksApp : public BaseApp {
public:
  void init() override;
  void loop() override;
  void redraw(bool force = false, int xOffset = 0) override;
  void setNeedsRedraw(bool flag) override;
  bool getNeedsRedraw() override;
  String getAppId() override { return "stocks"; }

private:
  int currentSymbolIndex = 0;
  unsigned long lastRotateAt = 0;
  bool needsRedraw = true;
};
