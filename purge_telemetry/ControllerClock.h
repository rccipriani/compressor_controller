#pragma once
#include "esp01/Protocol.h"

// Wall-clock history and command freshness; relay/debounce timing stays on millis().
// Extended monotonic milliseconds avoid losing elapsed time after 49.7 days.
class ControllerClock {
 public:
  bool clockValid = false;
  uint32_t syncEpoch = 0;
  uint64_t syncMillis = 0;
  uint64_t elapsedMillis = 0;
  uint32_t previousMillis = 0;
  int64_t lastCorrectionSeconds = 0;

  void tick(uint32_t now) {
    elapsedMillis += uint32_t(now - previousMillis);
    previousMillis = now;
    if (clockValid && epoch64() > UINT32_MAX) clockValid = false;
  }
  bool synchronize(uint32_t epoch, uint32_t now) {
    // Explicit supported sync range: 2024-01-01 through 2099-12-31 UTC.
    if (epoch < 1704067200UL || epoch >= 4102444800UL) return false;
    tick(now);
    lastCorrectionSeconds = clockValid ? int64_t(epoch) - int64_t(epoch64()) : 0;
    syncEpoch = epoch; syncMillis = elapsedMillis; clockValid = true;
    return true;
  }
  uint32_t epoch() const { return clockValid ? uint32_t(epoch64()) : 0; }
  uint32_t age() const {
    uint64_t seconds = (elapsedMillis - syncMillis) / 1000;
    return seconds > UINT32_MAX ? UINT32_MAX : uint32_t(seconds);
  }
 private:
  uint64_t epoch64() const { return uint64_t(syncEpoch) + (elapsedMillis - syncMillis) / 1000; }
};
ControllerClock processClock;
void handleTimeSync(uint32_t epoch) { processClock.synchronize(epoch, millis()); }
bool isClockValid() { processClock.tick(millis()); return processClock.clockValid; }
uint32_t getCurrentEpoch() { processClock.tick(millis()); return processClock.epoch(); }

// Future motor/fault histories can use this same epoch/valid pair.
// No EEPROM persistence: power loss invalidates the clock and clears history.
