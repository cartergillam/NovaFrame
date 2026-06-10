#pragma once

#include <Arduino.h>
#include <time.h>

struct DisplayTimeParts {
  String main = "--:--";
  String suffix = "";
};

class TimeCache {
public:
  void init();                    // Fetches and sets the current time
  void updateIfNeeded();         // Re-syncs time if 6 hours passed
  String getCurrentTimeString(); // Returns HH:MM:SS
  String getFormattedTime();
  DisplayTimeParts getDisplayTimeParts();
  int getHour();                 // Returns current hour
  int getMinute();               // Returns current minute
  int getWeekdayIndex();         // 0=Sun..6=Sat
  int getMinutesSinceMidnight();
  unsigned long getCurrentUnixTime();
  bool isSynchronized();

private:
  time_t baseUtcEpoch = 0;
  long utcOffsetSeconds = 0;
  unsigned long epochStartMillis = 0;
  unsigned long lastSync = 0;
  const unsigned long SYNC_INTERVAL = 21600000; // 6 hours
  const unsigned long FAILED_RETRY_INTERVAL = 60000; // 1 minute

  tm getCurrentLocalTm();
  bool fetchTime();  // Fetch time from API and update epoch
};
