#include "Arduino.h"
#include <assert.h>
#include <iostream>
uint32_t testMillis=0;
int pinStates[20]={};
void startPurge(unsigned long);
#include "../../airtankpurge.ino"

void runFor(uint32_t duration) {
  uint32_t start=testMillis;
  while(uint32_t(testMillis-start)<duration) { loop(); ++testMillis; }
}
void until(uint32_t point) { while(testMillis<point) { loop(); ++testMillis; } }

int main() {
  ControllerClock c;
  assert(!c.clockValid && c.epoch()==0);
  assert(!c.synchronize(0,0) && !c.synchronize(1699999999,0));
  assert(c.synchronize(1789088325,0));
  c.tick(UINT32_MAX-100);
  c.tick(899);
  assert(c.epoch()==1789088325UL+4294968UL);
  // A second full millis rollover during an outage is still accumulated.
  c.tick(UINT32_MAX-100); c.tick(899);
  assert(c.epoch()==1789088325UL+8589935UL);
  auto saved=c.epoch();
  assert(c.synchronize(1789088325,899) && c.lastCorrectionSeconds<0);
  assert(saved>c.epoch());

  LineReader<20> parser;
  for(char ch:std::string(30,'x')) assert(!parser.push(ch,0));
  assert(!parser.push('\n',0));
  for(char ch:std::string("CMD status")) assert(!parser.push(ch,1));
  assert(parser.push('\n',1) && !strcmp(parser.data,"CMD status"));
  parser.push('T',10); parser.push('I',3000);
  assert(!parser.push('\n',3000));
  uint32_t value;
  assert(parseDecimal("4294967295",value) && value==UINT32_MAX);
  assert(!parseDecimal("4294967296",value) && !parseDecimal("-1",value));

  pinStates[BUTTON_PIN]=HIGH;
  setup();
  assert(pinStates[RELAY_PIN]==HIGH);
  until(999); assert(startupTestActive && pinStates[RELAY_PIN]==HIGH);
  until(1001); assert(!startupTestActive && pinStates[RELAY_PIN]==LOW);
  assert(completedPurges==0 && !isClockValid());
  auto schedule=previousPurgeMillis;
  espLink.inject("CMD purge\nTIME 0\nTIME 4294967296\n");
  runFor(100);
  assert(!purgeActive && !isClockValid() && previousPurgeMillis==schedule);
  espLink.inject("TIME 1789088325\n");
  runFor(100); assert(isClockValid());
  // Bounce does not activate; stable press does.
  pinStates[BUTTON_PIN]=LOW; runFor(20);
  pinStates[BUTTON_PIN]=HIGH; runFor(60);
  assert(!purgeActive);
  pinStates[BUTTON_PIN]=LOW; runFor(60);
  assert(purgeActive && pinStates[RELAY_PIN]==HIGH);
  uint32_t start=purgeStartMillis;
  handleTimeSync(1800000000); // wall-clock jump during active purge
  assert(purgeStartMillis==start);
  assert(previousPurgeMillis==start);
  pinStates[BUTTON_PIN]=HIGH; runFor(60);
  pinStates[BUTTON_PIN]=LOW; runFor(60);
  assert(purgeStartMillis==start); // active press consumed, no extension
  until(start+4999); assert(purgeActive);
  until(start+5001); assert(!purgeActive && pinStates[RELAY_PIN]==LOW);
  assert(completedPurges==1 && lastPurgeDuration==5000);
  assert(lastPurgeTimeValid && lastPurgeEpoch>=1789088325);
  assert(!measurements.successValid);
  runFor(100); assert(!purgeActive); // held button cannot retrigger
  pinStates[BUTTON_PIN]=HIGH; runFor(60);
  // Automatic threshold uses monotonic timer, independent of TIME jumps.
  handleTimeSync(1800000000); assert(previousPurgeMillis==start);
  testMillis=start+INTERVAL_MS-1; loop(); assert(!purgeActive);
  testMillis=start+INTERVAL_MS; loop(); assert(purgeActive);
  // Shutoff subtraction remains correct across millis rollover.
  purgeStartMillis=UINT32_MAX-2500; testMillis=2498; loop(); assert(purgeActive);
  testMillis=2499; loop(); assert(!purgeActive);
  // Simulate a fresh controller boot with the manual button held.
  testMillis=0; processClock=ControllerClock(); controllerUptime=Uptime();
  startupTestActive=true; purgeActive=false; completedPurges=0;
  lastRemoteRequest=0; remoteBootFloor=0;
  stableButtonState=HIGH; lastButtonReading=HIGH;
  lastButtonChangeMillis=0; telemetryTx[0]=0; telemetryPos=0;
  lastLineEnd=0; lastTxByte=0; lastSnapshot=0;
  pinStates[BUTTON_PIN]=LOW; setup();
  until(999); assert(startupTestActive && !purgeActive);
  until(1060); assert(!startupTestActive && purgeActive);
  start=purgeStartMillis;
  until(start+5001);
  assert(completedPurges==1 && !lastPurgeTimeValid && lastPurgeEpoch==0);
  // Remote commands require clock validity and a post-boot timestamp.
  pinStates[BUTTON_PIN]=HIGH; runFor(60);
  espLink.inject("CMD purge 1800000000\n"); runFor(100); assert(!purgeActive);
  espLink.inject("TIME 1800000000\n"); runFor(3100);
  espLink.inject("CMD purge 1800000003\n"); runFor(100);
  assert(purgeActive && pinStates[RELAY_PIN]==HIGH);
  start=purgeStartMillis; assert(previousPurgeMillis==start);
  espLink.inject("CMD purge 1800000003\nCMD purge 1800000004\n"); runFor(100);
  assert(purgeStartMillis==start && lastRemoteRequest==1800000004);
  until(start+5001); assert(!purgeActive && lastPurgeDuration==5000);
  espLink.inject("CMD purge 1800000003\nCMD purge 1800000004\nCMD purge 1900000000\n");
  runFor(100); assert(!purgeActive && previousPurgeMillis==start);
  espLink.inject("CMD purge 1800000008\n"); runFor(100); assert(purgeActive);
  // A configuration change during a cycle must not alter its deadline.
  start=purgeStartMillis;
  espLink.inject("CMD duration 1800000009 1000\n"); runFor(100);
  assert(configuredPurgeDurationMs==1000 && activePurgeDurationMs==5000);
  assert(previousPurgeMillis==start);
  until(start+4999); assert(purgeActive);
  until(start+5001); assert(!purgeActive && lastPurgeDuration==5000);
  espLink.inject("CMD purge 1800000013\n"); runFor(100);
  assert(purgeActive && activePurgeDurationMs==1000);
  start=purgeStartMillis;
  until(start+999); assert(purgeActive);
  until(start+1001); assert(!purgeActive && lastPurgeDuration==1000);
  espLink.inject("CMD duration 1800000014 999\nCMD duration 1800000015 30001\n");
  runFor(100); assert(configuredPurgeDurationMs==1000 && !purgeActive);
  // Malformed commands and replay cannot change the setpoint.
  espLink.inject("CMD duration 1800000015 30000\nCMD duration 1800000016 -1\nCMD duration 1800000016 1000 extra\n");
  runFor(100); assert(configuredPurgeDurationMs==1000);
  runFor(2000);
  espLink.inject("CMD duration 1800000016 30000\n"); runFor(100);
  assert(configuredPurgeDurationMs==30000);
  pinStates[BUTTON_PIN]=LOW; runFor(60);
  assert(purgeActive && activePurgeDurationMs==30000);
  start=purgeStartMillis;
  until(start+29999); assert(purgeActive);
  until(start+30001); assert(!purgeActive && lastPurgeDuration==30000);
  pinStates[BUTTON_PIN]=HIGH; runFor(60);
  espLink.inject("CMD duration 1800000020 5000\n"); runFor(100);
  assert(configuredPurgeDurationMs==30000); // stale even though newer than last ID
  testMillis=previousPurgeMillis+INTERVAL_MS; loop();
  assert(purgeActive && activePurgeDurationMs==30000);
  // Direct calls also enforce startup interlock.
  purgeActive=false; startupTestActive=true; digitalWrite(RELAY_PIN,LOW);
  startPurge(testMillis); assert(!purgeActive && pinStates[RELAY_PIN]==LOW);
  std::cout<<"PASS: clock, framing, startup, debounce, remote validity/replay/busy/duration/schedule, timestamp, automatic timer, rollover\n";
}
