#pragma once
#include <SoftwareSerial.h>
#include "ControllerClock.h"
#include "DurationSettings.h"

// D2 RX <- adapter TX; D3 TX -> adapter RX. D7/D8/D13 untouched.
SoftwareSerial espLink(2, 3);
const char UNO_FIRMWARE[] = "1.4.1";
void startPurge(unsigned long currentMillis);
uint32_t lastRemoteRequest = 0, remoteBootFloor = 0;
char commandAck[64] = "";
uint32_t configuredPurgeDurationMs = DEFAULT_PURGE_DURATION_MS;
bool configReportPending = true;
Uptime controllerUptime;
LineReader<40> commandLine;
uint32_t completedPurges = 0, lastPurgeDuration = 0;
bool completionPending = false, statusRequested = true;
char telemetryTx[240];
bool clockReportPending = false, timeRequestSent = false;
uint32_t lastTimeRequest = 0, lastLineEnd = 0;
uint32_t lastPurgeEpoch = 0;
bool lastPurgeTimeValid = false;
uint8_t telemetryPos = 0;
uint32_t lastSnapshot = 0, lastTxByte = 0;

// Future hooks: valid only after actual sensor measurements/assessment.
struct ControllerMeasurements {
  bool pressureValid = false, rpmValid = false, motorValid = false;
  bool successValid = false, success = false, motorRunning = false;
  long pressureMilliBar = 0;
  unsigned long rpm = 0;
} measurements;

void recordPurgeComplete(uint32_t duration) {
  lastPurgeTimeValid = isClockValid();
  lastPurgeEpoch = lastPurgeTimeValid ? getCurrentEpoch() : 0;
  ++completedPurges;
  lastPurgeDuration = duration;
  // TODO: Uno pressure-response assessment before setting successValid.
  measurements.successValid = false;
  completionPending = true; // latest event only; snapshot recovers count/duration
  statusRequested = true;
}

void serviceTelemetry(uint32_t now, bool startup, bool active,
                      uint32_t deadlineStart, uint32_t deadlineDuration) {
  controllerUptime.tick(now);
  processClock.tick(now);
  // Stop RX interrupts and telemetry near either relay shutoff deadline.
  if ((startup || active) && uint32_t(now - deadlineStart) >= deadlineDuration - 10) {
    espLink.stopListening();
    return;
  }
  espLink.listen();
  for (byte budget = 0; budget < 8 && espLink.available(); ++budget) {
    if (commandLine.push(espLink.read(), now)) {
      if (!strcmp(commandLine.data, "CMD status")) statusRequested = true;
      uint32_t epoch;
      if (!strncmp(commandLine.data, "TIME ", 5) &&
          parseDecimal(commandLine.data + 5, epoch)) {
        handleTimeSync(epoch);
        clockReportPending = true;
        if (isClockValid() && !remoteBootFloor) remoteBootFloor = getCurrentEpoch() + 2;
      }
      bool durationCommand = !strncmp(commandLine.data, "CMD duration ", 13);
      uint32_t requestedDuration = 0;
      if ((durationCommand || !strncmp(commandLine.data, "CMD purge ", 10)) &&
          parseRemoteRequest(commandLine.data + (durationCommand ? 13 : 10),
                             durationCommand, epoch, requestedDuration)) {
        const char* result;
        uint32_t current = getCurrentEpoch();
        if (isClockValid() && !remoteBootFloor) remoteBootFloor = current + 2;
        if (!isClockValid()) result = "clock_invalid";
        else if (epoch <= remoteBootFloor || epoch <= lastRemoteRequest) result = "replay";
        else if (int64_t(current) - epoch > 10 || int64_t(epoch) - current > 2) result = "expired";
        else {
          // Rejected requests are consumed, never queued for later actuation.
          lastRemoteRequest = epoch;
          if (durationCommand) {
            if (!validPurgeDuration(requestedDuration)) result = "out_of_range";
            else {
              configuredPurgeDurationMs = requestedDuration;
              configReportPending = true; result = "accepted";
            }
          } else if (startup) result = "startup";
          else if (active) result = "busy";
          else { startPurge(millis()); active = true; result = "accepted"; }
        }
        snprintf(commandAck, sizeof commandAck, "ACK %s %lu %s\n",
                 durationCommand ? "duration" : "purge", (unsigned long)epoch, result);
      }
    }
  }
  if (!telemetryTx[telemetryPos]) {
    if (uint32_t(now - lastLineEnd) < 100) return; // peer reply window
    telemetryPos = 0;
    if (!processClock.clockValid && (!timeRequestSent || uint32_t(now-lastTimeRequest)>=30000)) {
      strcpy(telemetryTx, "GET TIME\n");
      timeRequestSent = true; lastTimeRequest = now;
    } else if (commandAck[0]) {
      strcpy(telemetryTx, commandAck); commandAck[0] = 0;
    } else if (completionPending) {
      snprintf(telemetryTx, sizeof telemetryTx,
        "EVENT purge_complete count=%lu duration_ms=%lu success=%s epoch=%lu time_valid=%u\n",
        (unsigned long)completedPurges, (unsigned long)lastPurgeDuration,
        measurements.successValid ? (measurements.success ? "true" : "false") : "unknown",
        (unsigned long)lastPurgeEpoch, lastPurgeTimeValid ? 1U : 0U);
      completionPending = false;
    } else if (statusRequested || uint32_t(now - lastSnapshot) >= 5000) {
      snprintf(telemetryTx, sizeof telemetryTx,
        "STAT uptime=%lu state=%s count=%lu duration_ms=%lu success=%s firmware=%s epoch=%lu time_valid=%u\n",
        (unsigned long)controllerUptime.seconds, startup ? "STARTUP" : active ? "PURGING" : "IDLE",
        (unsigned long)completedPurges, (unsigned long)lastPurgeDuration,
        measurements.successValid ? (measurements.success ? "true" : "false") : "unknown", UNO_FIRMWARE,
        (unsigned long)lastPurgeEpoch, lastPurgeTimeValid ? 1U : 0U);
      lastSnapshot = now; statusRequested = false; clockReportPending = true; configReportPending = true;
    } else if (configReportPending) {
      snprintf(telemetryTx, sizeof telemetryTx, "CONFIG duration_ms=%lu\n",
               (unsigned long)configuredPurgeDurationMs);
      configReportPending = false;
    } else if (clockReportPending) {
      snprintf(telemetryTx, sizeof telemetryTx,
        "CLOCK valid=%u sync_epoch=%lu sync_age=%lu correction=%ld\n",
        processClock.clockValid ? 1U : 0U, (unsigned long)processClock.syncEpoch,
        (unsigned long)processClock.age(),
        (long)(processClock.lastCorrectionSeconds > INT32_MAX ? INT32_MAX :
               processClock.lastCorrectionSeconds < INT32_MIN ? INT32_MIN : processClock.lastCorrectionSeconds));
      clockReportPending = false;
    } else return;
  }
  // SoftwareSerial TX is synchronous: only one ~1.04 ms byte per call,
  // spaced 3 ms apart. No flush(), blocking handshake, or wait for a peer.
  if (uint32_t(now - lastTxByte) >= 3) {
    espLink.write(telemetryTx[telemetryPos++]);
    lastTxByte = now;
    if (!telemetryTx[telemetryPos]) lastLineEnd = now;
  }
}
