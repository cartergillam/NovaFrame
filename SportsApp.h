#pragma once

#include "BaseApp.h"

class SportsApp : public BaseApp {
public:
  explicit SportsApp(const String& leagueId, const String& leagueLabel);
  void init() override;
  void loop() override;
  void redraw(bool force = false, int xOffset = 0) override;
  void setNeedsRedraw(bool flag) override;
  bool getNeedsRedraw() override;
  String getAppId() override { return leagueId; }

private:
  String leagueId;
  String leagueLabel;
  int currentFavoriteIndex = 0;
  unsigned long lastRotateAt = 0;
  bool needsRedraw = true;
};
