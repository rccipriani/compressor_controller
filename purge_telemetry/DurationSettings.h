#pragma once
#include <EEPROM.h>
#include "esp01/Protocol.h"

// Two 8-byte records at EEPROM addresses 0..15. Commit marker is written last.
// Inverted fields validate the sequence and duration; version permits migration.
class DurationSettings {
  int slot = -1;
  uint8_t sequence = 0;
  uint32_t saved = DEFAULT_PURGE_DURATION_MS;
  bool readSlot(int index, uint8_t& seq, uint32_t& value) {
    int base = index * 8;
    uint8_t data[8];
    for (uint8_t i=0; i<8; ++i) data[i]=EEPROM.read(base+i);
    if (data[0]!=0xA5 || data[1]!=1 || uint8_t(~data[2])!=data[5] ||
        uint8_t(~data[3])!=data[6] || uint8_t(~data[4])!=data[7]) return false;
    seq=data[2]; value=uint32_t(data[3]) | (uint32_t(data[4])<<8);
    return validPurgeDuration(value);
  }
 public:
  uint32_t load() {
    slot=-1; sequence=0; saved=DEFAULT_PURGE_DURATION_MS;
    for (int i=0; i<2; ++i) {
      uint8_t seq; uint32_t value;
      if (readSlot(i,seq,value) && (slot<0 || (uint8_t(seq-sequence)>0 && uint8_t(seq-sequence)<128))) {
        slot=i; sequence=seq; saved=value;
      }
    }
    return saved;
  }
  // Call only with the relay idle. EEPROM.update skips bytes already matching.
  void save(uint32_t value) {
    if (!validPurgeDuration(value) || value==saved) return;
    int next=slot==0 ? 1 : 0;
    uint8_t seq=uint8_t(sequence+1), lo=uint8_t(value), hi=uint8_t(value>>8);
    uint8_t data[8]={0xA5,1,seq,lo,hi,uint8_t(~seq),uint8_t(~lo),uint8_t(~hi)};
    int base=next*8;
    EEPROM.update(base,0); // Invalidate destination; keep previous record intact.
    for (uint8_t i=1; i<8; ++i) EEPROM.update(base+i,data[i]);
    EEPROM.update(base,data[0]);
    slot=next; sequence=seq; saved=value;
  }
};
DurationSettings durationSettings;
