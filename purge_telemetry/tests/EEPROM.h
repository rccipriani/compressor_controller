#pragma once
#include <stdint.h>
#include <string.h>
struct FakeEEPROM {
  uint8_t bytes[1024];
  int writes=0, calls=0, failAfter=-1;
  FakeEEPROM() { memset(bytes,255,sizeof bytes); }
  uint8_t read(int address) { return bytes[address]; }
  void update(int address,uint8_t value) {
    if (failAfter==calls++) throw 1;
    if (bytes[address]!=value) { bytes[address]=value; ++writes; }
  }
} EEPROM;
