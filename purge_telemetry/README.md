# Compressor purge telemetry

The Uno remains the only physical controller. D7 relay (active HIGH), D8 manual button (INPUT_PULLUP, pressed LOW), D13 LED, 1-second startup test, 5-second default purge (remotely configurable from 1 to 30 seconds), 24-hour interval, 50-ms debounce and LED intervals are unchanged. Startup resets the interval at test completion; automatic/manual/remote cycles reset it at cycle start. A held button at power-up still triggers a full purge after startup and debounce. A press during a purge is consumed without extending that purge. Startup is not counted as a normal purge.

## Files and dependencies

- ../airtankpurge.ino: original controller plus telemetry initialization and hooks.
- UnoTelemetry.h: bounded SoftwareSerial service, completion counter and future sensor hooks.
- ControllerClock.h: modular Uno wall clock with validity and extended monotonic reference.
- esp01/Protocol.h: shared fixed-buffer line reader, decimal parser and uptime accumulation.
- esp01/esp01.ino: custom ESP8266 gateway.
- esp01/config.h: placeholder network/broker/NTP configuration.
- tests/: host control/protocol regression checks.
- build.ps1: stages separate sketch folders; the repository root contains unrelated .ino files and must not be compiled as one sketch.

Uno: Arduino AVR Boards and bundled SoftwareSerial. ESP: ESP8266 Arduino core 3.1.2, AsyncMqttClient 0.9.0, ESP Async TCP 2.0.0. WiFi and time APIs are bundled with the ESP core. Async MQTT avoids blocking broker connection attempts. Telemetry uses at most one unacknowledged QoS1 publication, with a 15-second disconnect timeout. There is no expanding offline event queue.

Configure esp01/config.h locally. Do not commit actual credentials. Defaults use plain MQTT on a trusted local network; configure broker credentials/ACLs as appropriate. Base topic must be shorter than 79 characters. Use a different base for each controller; ESP client ID derives from chip ID.

## Wiring

With power off, connect through the stated adapter's **5 V logic side**:

| Uno | Adapter / ESP |
|---|---|
| D2 (SoftwareSerial RX) | shifted TX output from ESP GPIO1/TX |
| D3 (SoftwareSerial TX) | shifted RX input to ESP GPIO3/RX |
| GND | adapter GND |
| suitable regulated 5 V supply | adapter 5 V input |

D2 and D3 were unused in the inspected sketch; no existing assignments move. They are suitable for Uno SoftwareSerial, but are also the Uno external-interrupt pins: revisit this allocation if a future RPM sensor needs an external interrupt.

Adapter labels differ: verify signal direction, rather than assuming a label is from the Uno's perspective. Use the adapter's regulation and level shifting; do not connect raw ESP pins to 5 V. Size the 5 V supply for WiFi current bursts and the adapter rating. The existing relay driver, flyback protection, and solenoid wiring are unchanged.

ESP normal boot requires EN/CH_PD and reset HIGH, GPIO0 HIGH and GPIO2 HIGH (normally handled by the adapter/pullups). For flashing, use an ESP programmer/appropriate USB-UART adapter and pull GPIO0 LOW during reset. Disconnect Uno UART connections while flashing to prevent two transmitters driving the line. ESP boot ROM output at 74880 baud is expected; the Uno rejects non-command text. D0/D1 remain free for Uno USB programming/debugging. No ESP debug output shares the telemetry UART.

## UART protocol

9600 baud, 8N1, ASCII, LF terminator (CRLF accepted). Fields are ordered for this firmware pair (Uno 1.4.0, ESP 1.2.0). Unknown message types, malformed fields, oversized lines, binary characters and partial lines timing out after 2 seconds are discarded. Receivers recover at the next LF. Add new record types for future sensor data; do not silently append fields to STAT without updating its parser.

Examples:

```text
STAT uptime=123 state=IDLE count=2 duration_ms=5000 success=unknown firmware=1.4.0 epoch=1789088325 time_valid=1
STAT uptime=128 state=PURGING count=2 duration_ms=5000 success=unknown firmware=1.4.0 epoch=1789088325 time_valid=1
EVENT purge_complete count=3 duration_ms=5000 success=unknown epoch=1789088330 time_valid=1
CLOCK valid=1 sync_epoch=1789088325 sync_age=5 correction=0
GET TIME
TIME 1789088330
CMD status
CMD purge 1789088330
CMD duration 1789088336 7500
ACK purge 1789088330 accepted
ACK duration 1789088336 accepted
CONFIG duration_ms=7500
```

STAT is sent every 5 seconds and requested at state changes, followed by CLOCK diagnostics; EVENT is a best-effort completion record. UART uptime is seconds, duration is milliseconds, count is completed automatic/manual/remote cycles since Uno reset. No EEPROM persistence is claimed. STARTUP reports purge/active=true because the valve is energized even though it is not a counted cycle.

CMD status requests a fresh snapshot. CMD purge carries a 10-digit UTC Unix timestamp used as its request ID. CMD duration carries the timestamp followed by a duration in integer milliseconds. The Uno checks clock validity, freshness, increasing request IDs, and local purge interlocks. It sends ACK with the request kind, ID and result; CONFIG reports the current duration after each STAT and after accepted changes. CONFIG does not extend STAT's existing fields.

The Uno leaves a 100-ms gap between lines; the ESP waits for 20-ms receive silence before sending commands. SoftwareSerial is half-duplex, so delivery is best-effort. Commands have no automatic retry; an unsent gateway request expires after one second and is discarded on MQTT disconnect. Lost status/config reports recover through periodic telemetry. GET TIME retries every 30 seconds until the Uno clock becomes valid.

The Uno has a 240-byte outgoing line, a 40-byte command buffer and one pending latest completion. It sends one character per loop, spaced 3 ms apart, after physical control logic. SoftwareSerial still occupies approximately 1.04 ms per transmitted byte and masks interrupts during most of that byte; this is bounded, not genuinely asynchronous TX. RX/listening and telemetry stop in the final 10 ms before startup/purge shutoff, then resume afterward. No communication handshake or network state gates control. Bench-check timing with the actual wiring; SoftwareSerial cannot promise zero CPU/interrupt overhead under arbitrary electrical noise.

## MQTT topics

All paths below are relative to **compressor/purge_controller** (configurable).

| Suffix | Payload / units |
|---|---|
| status/online | true; retained LWT false |
| status/state | STARTUP, IDLE, PURGING, UNKNOWN |
| status/uptime | ESP uptime, seconds |
| status/wifi_rssi | dBm integer |
| status/firmware | ESP firmware version |
| controller/available | true if a valid STAT arrived within 15 seconds |
| controller/uptime | Uno uptime, seconds |
| controller/firmware | Uno version |
| purge/active | true / false; includes startup valve activation |
| purge/count | completed cycles since Uno reset |
| purge/last_duration | milliseconds; absent until first completion |
| purge/last_success | true / false, only when valid |
| purge/last_success_valid | currently false |
| purge/last_time | ISO-8601 UTC Uno-owned completion time, when valid |
| purge/last_time_valid | whether last_time belongs to the current last cycle |
| motor/running | reserved; cleared, no fabricated measurement |
| motor/running_valid | currently false |
| motor/rpm | reserved; cleared |
| motor/rpm_valid | currently false |
| tank/pressure | reserved; cleared; future unit: bar |
| tank/pressure_valid | currently false |
| command/purge_enabled | true: gateway supports remote purge; check availability/time/state separately |
| purge/duration_ms | Uno-confirmed configured duration in milliseconds, 1000–30000; cleared when unavailable/stale |
| purge/last_epoch | Uno-owned Unix completion epoch, when valid |
| time/valid | Uno clock valid, gated by fresh diagnostics and controller availability |
| time/last_sync | last epoch received by Uno in a valid TIME message |
| time/sync_age | Uno seconds since that synchronization |
| time/last_correction | signed correction in seconds (saturated int32 diagnostic) |
| time/esp_synchronized | true only after ESP receives an NTP time update |
| time/ntp_sync_age | ESP seconds since last NTP update |
| command/status | inbound non-retained payload request |
| command/purge | inbound non-retained payload: `<epoch>` |
| command/duration | inbound non-retained payload: `<epoch> <duration_ms>` |
| command/purge_result | non-retained response: `<epoch> <result>` |
| command/duration_result | non-retained response: `<epoch> <result>` |

State topics use retained QoS1; command results use non-retained QoS1. Invalid scalar measurements are cleared with zero-length retained publications (MQTT retained-message deletion); explicit valid flags are false. The gateway rotates through topics every 100 ms subject to broker acknowledgments: a normal full refresh takes about 3 seconds. Topics are not an atomic transaction. Consumers must check status/online, controller/available and individual valid flags before using retained values.

The retained LWT sets status/online=false after an unexpected disconnect is detected by the broker (keepalive 15 seconds; broker detection is not instantaneous). On connection, the first telemetry publication sets it true. It describes the ESP, not the Uno. When the Uno goes silent, availability becomes false after 15 seconds plus a topic refresh; stale controller numeric values are cleared and state becomes UNKNOWN. Old retained values may be visible briefly during reconnection.

## Time, restarts and outages

The Uno starts clock-invalid. The ESP starts NTP-unsynchronized, even if its system clock appears plausible. Only its SNTP time-update callback authorizes TIME transmission. NTP refresh uses the ESP8266 core's periodic SNTP service; reconnecting WiFi restarts configuration to request synchronization. ESP sends TIME after each valid NTP update, every 10 minutes afterward, and in response to GET TIME. It can continue sending extrapolated system time during a WiFi outage; time/ntp_sync_age exposes how old its NTP reference is. Neither board expires an initialized clock merely because the network is down.

TIME carries decimal UTC Unix seconds. The Uno strictly rejects malformed, overflowing and out-of-range values; supported synchronization epochs are 2024-01-01 through 2099-12-31 UTC. The clock API is handleTimeSync(epoch), isClockValid(), getCurrentEpoch(). It stores syncEpoch and an extended monotonic syncMillis reference, derives time using elapsed milliseconds divided by 1000, and never updates a timestamp in a per-second loop. A 64-bit monotonic accumulator extends unsigned millis deltas, surviving multiple 49.7-day rollovers provided the loop keeps running. getCurrentEpoch returns zero when invalid; consumers MUST check validity and never present zero as a real event time.

At timed purge completion, the Uno records lastPurgeEpoch plus lastPurgeTimeValid. Count/duration still update when time is invalid; success remains unknown without sensor evidence. The timestamp refers to cycle completion, not proven physical success. Both EVENT and periodic STAT carry this history, so a dropped event or ESP reboot can recover the exact Uno-owned timestamp later. The ESP only formats that epoch with gmtime_r/strftime; it never substitutes receipt time or retrospectively invents timestamps. Invalid history uses epoch=0 time_valid=0 on UART; MQTT clears its timestamp value.

New TIME messages replace the synchronization reference directly. time/last_correction reports the signed jump for diagnostics; historical timestamps already recorded stay unchanged, and all purge/debounce/interlock timers remain on millis. Wall-clock formatting is entirely on the ESP. Internal time and published ISO-8601 timestamps are UTC; TIMEZONE config is POSIX TZ for the ESP and never changes Uno data.

Reboot behavior:
- Both boards restart: Uno clock/history begin invalid, controls work immediately, GET TIME retries until ESP synchronizes.
- ESP alone restarts: Uno keeps approximate time and its history; ESP waits for real NTP sync before sending TIME, while recovering history from STAT.
- WiFi disappears: local operation/time extrapolation continue. On return ESP refreshes NTP and sends corrected time.
- Uno alone restarts: its clock/history reset; GET TIME obtains the already-synchronized ESP clock, typically within the first telemetry exchange (30-second retry if lost).

No EEPROM writes or persistence are added. Future motor/fault records can use the same epoch-plus-valid pattern in ControllerClock.h. There is no long-outage precision guarantee: both clocks can drift without new NTP information.

ESP retries WiFi every 30 seconds and MQTT at 5-second intervals, without rebooting or blocking for connection. It caches only the latest state and completion, coalescing intermediate history during outages. Power loss clears this RAM. After ESP reboot, Uno snapshots restore history while the Uno remains powered. A full Uno power loss clears history; no boot identity or event audit log is implemented.

Uptime accumulation survives the 49.7-day millis rollover; seconds wrap after about 136 years. The existing control timer subtraction remains rollover safe. UART has no checksum or delivery guarantee: this is best-effort supervision, not an audit log. Disconnected ESP/WiFi/broker cannot prevent a startup, automatic or local manual purge.

## Future sensors and commands

ControllerMeasurements in UnoTelemetry.h reserves validity and values for pressure (millibar), RPM, motor running and success. These are interfaces only, not live telemetry. Implement acquisition and purge assessment on the Uno, including freshness and sensor fault detection. Add bounded sensor records and ESP parsing, then replace the gateway's hardcoded false sensor-valid flags and clear-only measurement cases. Convert millibar to decimal bar on the ESP. Only set successValid when the Uno can assess the completed purge from real evidence; a timer ending is not proof of success.

## Remote purge and duration configuration

Flash both updated sketches together. The Uno remains responsible for the relay and all timing. MQTT purge uses the same action as the local button, resets the 24-hour schedule, and is rejected during startup or an active cycle. There are no pressure/motor interlocks yet because those sensors are not implemented.

The configured duration defaults to **5000 ms**, accepts **1000 through 30000 ms**, and applies to subsequent automatic, manual and MQTT cycles. A cycle latches its duration at start; changing the setting cannot shorten or extend it. The one-second startup test is unchanged. Settings are RAM-only: an Uno reboot restores 5000 ms, while an ESP reboot recovers the current setting from CONFIG. Setting duration alone does not energize the relay or reset the automatic schedule. `purge/last_duration` continues to report the measured duration of the most recently completed cycle.

Commands use QoS0 subscriptions and must be non-retained. Use the publisher's current UTC Unix seconds as the 10-digit request ID, and a strictly newer ID for each command across BOTH command topics. Both boards reject timestamps older than 10 seconds or more than 2 seconds ahead; the gateway accepts at most one command per five seconds. Both boards need synchronized clocks. The Uno also requires the ID to be later than its initial synchronization epoch plus two seconds, rejecting requests originating before its boot. Wait at least three seconds after initial synchronization before commanding it. Retained, fragmented, malformed, unavailable-controller, stale, duplicate and out-of-range gateway requests are discarded.

The Uno consumes fresh IDs even for rejected busy/startup/out-of-range commands. Responses are `accepted`, `busy`, `startup`, `clock_invalid`, `replay`, `expired`, or `out_of_range`. A result means the Uno processed a request; `accepted` for purge means the cycle started, not that a physical purge succeeded. Results are best-effort and not retained. If a response is missing, check state/count or the reported duration; do not automatically create a new purge ID to retry. Replay tracking is in RAM and assumes reasonably correct time synchronization; it is not a durable authenticated command ledger. A backward clock correction may prevent new requests until timestamps exceed the previous ID.

Broker authentication and topic ACLs provide authorization; timestamps are not credentials. Limit publishing on the two command topics to authorized operators, and use the existing trusted local network configuration. Do not publish retained commands. No credentials or broker configuration are changed by this update.

PowerShell examples (replace `BROKER` and supply your broker's authentication options):

```powershell
# Watch state and acknowledgments in a separate terminal.
mosquitto_sub -h BROKER -t 'compressor/purge_controller/#' -v

# Start one purge. Generate the timestamp immediately before publishing.
$requestEpoch = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
mosquitto_pub -h BROKER -q 0 -t 'compressor/purge_controller/command/purge' -m "$requestEpoch"

# Separately, at least five seconds after the previous command:
# configure subsequent purges for 7.5 seconds.
$requestEpoch = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
mosquitto_pub -h BROKER -q 0 -t 'compressor/purge_controller/command/duration' -m "$requestEpoch 7500"
```

Confirm `command/duration_result` reports the matching ID with `accepted`, then verify `purge/duration_ms` becomes `7500`. The existing retained `purge/last_duration` value is historical and need not change until another purge completes.

## Bench test before cabinet installation

1. Start with the physical valve disconnected, a test LED or meter on the existing relay driver, and an adequately powered ESP adapter. Verify voltages and UART directions.
2. Build both targets with build.ps1. Program Uno and ESP separately; restore UART wiring afterward. Set local config placeholders first.
3. With ESP disconnected, verify startup HIGH for 1 second; afterward button LOW for at least 50 ms triggers 5 seconds HIGH. Verify holding/releasing, bouncing, power-up held button, and presses during an active purge. Confirm normal/fast LED intervals remain 1000/125 ms per toggle.
4. Measure relay timings with a scope/logic analyzer with telemetry connected and disconnected, including noisy/malformed UART input. Verify malformed or replayed UART commands cannot cause early shutoff, retrigger, or timer reset.
5. Watch compressor/purge_controller/# using an MQTT client. Check versions, uptimes, RSSI, state, count and availability. Startup should not increment count; a finished manual cycle should increment it once. Success and sensor values must remain unavailable.
6. Let NTP synchronize, finish a cycle, and check last_time against UTC and last_duration against the measured cycle. Repeat after BOTH boards cold boot with NTP blocked: control works, time/valid and last_time_valid remain false. An ESP-only reboot must preserve the Uno clock/history.
7. Drop WiFi and stop the broker separately during idle and during purge. Operate the manual button; timing remains unchanged. Restore connectivity and verify reconnect/current state. Remove ESP power to observe retained LWT=false after broker detection.
8. Disconnect Uno TX while ESP stays online. After 15 seconds plus refresh, controller/available=false and state UNKNOWN. Restore it and confirm recovery.
9. Publish non-retained request to command/status; confirm fresh UART STAT. After clock synchronization, send a fresh timestamp to command/purge and verify exactly one configured cycle. Replay the same timestamp and send retained, stale, malformed and busy requests: none may start another cycle or extend the current one. Set duration to 1000 and 30000 ms, verify rejection of 999 and 30001, and measure actual relay timing. Change duration mid-cycle and verify only the next cycle uses it. Reboot the Uno and verify the setting returns to 5000 ms.
10. Reset each board separately and check documented counter/timestamp recovery. Test multiple offline cycles: only the latest state is retained, not a history.
11. Verify the real 24-hour automatic interval and manual reset of that schedule. A temporary bench-only shortened interval can speed initial testing; restore 24 hours and rebuild before installation. Exercise millis rollover with a test harness.
12. Only after electrical and timing checks pass, reconnect the valve for a supervised functional test; physical purge success still requires observation until sensing exists.

Library references: [AVR SoftwareSerial source](https://github.com/arduino/ArduinoCore-avr/blob/master/libraries/SoftwareSerial/src/SoftwareSerial.cpp), [AsyncMqttClient](https://github.com/marvinroger/async-mqtt-client), [ESPAsyncTCP](https://github.com/ESP32Async/ESPAsyncTCP), [ESP8266 core](https://github.com/esp8266/Arduino).

## Additional time bench checks

1. Uno alone: clock invalid, startup/manual/automatic purge work, invalid history epoch is not published as a date.
2. ESP connected with no WiFi from cold boot: no TIME is transmitted; GET TIME cannot manufacture validity.
3. Enable WiFi/NTP: capture TIME, CLOCK valid=1, time/esp_synchronized=true and time/valid=true.
4. Complete a purge: compare Uno event epoch, retained last_epoch and ISO-8601 last_time. Success remains unavailable.
5. Remove WiFi for several cycles: Uno timestamps continue advancing; reconnect and verify retained history comes from Uno.
6. Reconnect WiFi: check new TIME and sync-age reset; neither a forward nor a backward clock correction changes a running purge or its automatic schedule.
7. Reboot only ESP: Uno clock stays valid; gateway regains exact history from STAT before or after its own NTP returns.
8. Reboot only Uno: clock starts invalid, GET TIME restores it shortly; historical count/time reset.
9. Inject TIME 0, negative values, values beyond uint32, nonnumeric and overlong lines: clock reference must not change.
10. In the host regression harness, exercise multiple millis rollovers, backward clock corrections and a clock-invalid completion. Hardware tests are still needed for electrical noise, interrupt latency, broker LWT and actual valve operation.

## Building and regression checks

Install the board/library versions above, then run:

```powershell
arduino-cli core install arduino:avr@1.8.8
arduino-cli core install esp8266:esp8266@3.1.2 --additional-urls https://arduino.esp8266.com/stable/package_esp8266com_index.json
arduino-cli lib install "AsyncMqttClient@0.9.0" "ESP Async TCP@2.0.0"
./purge_telemetry/build.ps1
```

Generic ESP8266 defaults may not match your ESP-01 flash size; check the module and select the correct board options using build.ps1 -EspBoard before upload. No hardware flashing is performed by the build script.

The host tests compile the actual sketch/header code against small fake I/O adapters. In a Visual Studio x64 Developer Command Prompt, run from the repository root:

```text
cl /nologo /EHsc /std:c++14 /Ipurge_telemetry/tests purge_telemetry/tests/control_test.cpp /Fe:.purge-build/control_test.exe /Fo:.purge-build/control_test.obj
.purge-build/control_test.exe
cl /nologo /EHsc /std:c++14 /Ipurge_telemetry/tests/esp /Ipurge_telemetry/tests purge_telemetry/tests/esp/gateway_test.cpp /Fe:.purge-build/gateway_test.exe /Fo:.purge-build/gateway_test.obj
.purge-build/gateway_test.exe
```

Create .purge-build first, or run build.ps1. These mocks verify application behavior, not radio/library timing or electronics.

## Verification performed

- Current controller and gateway host harnesses compile with MSVC and pass. They exercise clock rollover/corrections, framing, startup/debounce, automatic timing, remote freshness/replay/busy behavior, acknowledgment forwarding, duration bounds, reported configuration, command expiry, and duration changes during active purges.
- Current Arduino board builds could not run: Windows Application Control blocked the installed `arduino-cli.exe`. The older firmware's successful compilation does not verify this revision's flash/RAM usage or board compatibility.
- No boards were flashed and no real broker, UART, relay or valve test was performed. Complete the bench procedure after building both updated targets.
