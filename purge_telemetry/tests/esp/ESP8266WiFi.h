#pragma once
#include "../Arduino.h"
#include <string>
#include <deque>
#include <time.h>
#include <functional>
constexpr int WL_CONNECTED=3, WIFI_STA=1;
struct FakeWiFi {
 int state=WL_CONNECTED;
 int status(){return state;} int RSSI(){return -60;}
 void persistent(bool){} void mode(int){} void setAutoReconnect(bool){}
 void begin(const char*,const char*){} void reconnect(){}
} WiFi;
struct FakeESP { unsigned int getChipId(){return 123;} } ESP;
struct FakeSerial {
 std::string tx; std::deque<char> rx;
 void setRxBufferSize(int){} void begin(int){}
 int available(){return int(rx.size());}
 int read(){char c=rx.front();rx.pop_front();return c;}
 int availableForWrite(){return 128;}
 size_t write(const char* s){tx+=s;return strlen(s);}
 size_t write(const uint8_t* s,size_t n){tx.append((const char*)s,n);return n;}
} Serial;
inline void configTime(const char*,const char*){}
inline void yield(){}
inline tm* gmtime_r(const time_t* t,tm* out){return gmtime_s(out,t)==0?out:nullptr;}

// Deterministic SNTP clock for freshness and replay regression tests.
inline time_t testTime(time_t* out) {
  time_t value=1800000000UL + testMillis/1000;
  if (out) *out=value;
  return value;
}
#define time testTime
