#pragma once

#include <Arduino.h>
#include <time.h>

class TimeCache {
public:
  void init();                    // Fetches and sets the current time
  void updateIfNeeded();         // Re-syncs time if 6 hours passed
  String getCurrentTimeString(); // Returns HH:MM:SS
  String getFormattedTime();     // Formatted based on user preference
  int getHour();                 // Returns current hour
  int getMinute();               // Returns current minute

private:
  time_t baseEpoch = 0;
  unsigned long epochStartMillis = 0;
  unsigned long lastSync = 0;
  const unsigned long SYNC_INTERVAL = 21600000; // 6 hours

  void fetchTime();  // Fetch time from API and update epoch
};