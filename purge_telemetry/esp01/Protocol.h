#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <string.h>

// Invalid/oversize/partial lines are discarded through the next newline.
template <size_t N> struct LineReader {
  char data[N];
  size_t used = 0;
  bool discard = false;
  uint32_t lastByte = 0;
  bool push(char c, uint32_t now) {
    if (used && uint32_t(now - lastByte) > 2000) discard = true;
    lastByte = now;
    if (c == '\n') {
      bool ready = !discard && used;
      data[used] = 0;
      used = 0; discard = false;
      return ready;
    }
    if (c == '\r') return false;
    if (c < 32 || c > 126 || used >= N - 1) discard = true;
    if (!discard) data[used++] = c;
    return false;
  }
};
struct Uptime {
  uint32_t previous = 0, seconds = 0, remainder = 0;
  void tick(uint32_t now) {
    uint32_t delta = now - previous;
    previous = now;
    seconds += delta / 1000;
    remainder += delta % 1000;
    if (remainder >= 1000) { ++seconds; remainder -= 1000; }
  }
};

// Strict decimal parser, including uint32 overflow rejection.
inline bool parseDecimal(const char* text, uint32_t& value) {
  if (!*text) return false;
  uint32_t n = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') return false;
    uint8_t digit = *text - '0';
    if (n > (UINT32_MAX - digit) / 10) return false;
    n = n * 10 + digit;
  }
  value = n; return true;
}

// Shared bounds are enforced again by the physical controller.
const uint32_t DEFAULT_PURGE_DURATION_MS = 10000;
const uint32_t MIN_PURGE_DURATION_MS = 1000;
const uint32_t MAX_PURGE_DURATION_MS = 30000;
inline bool validPurgeDuration(uint32_t value) {
  return value >= MIN_PURGE_DURATION_MS && value <= MAX_PURGE_DURATION_MS;
}
// Timestamp is the request ID. Duration commands append one decimal value.
inline bool parseRemoteRequest(char* text, bool durationCommand, uint32_t& epoch, uint32_t& duration) {
  size_t length = strlen(text);
  if (durationCommand) {
    if (length < 12 || length > 16 || text[10] != ' ') return false;
    text[10] = 0;
    if (!parseDecimal(text + 11, duration)) return false;
  } else if (length != 10) return false;
  return parseDecimal(text, epoch);
}
