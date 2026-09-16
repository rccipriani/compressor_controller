// Project: Compressor Auto Purge Controller
// Author: Robert Cipriani
// Last Updated: 2026-09-16
//
// v1.4.0
//
// Hardware:
// - Arduino UNO
// - ASCO 1/4" 8262H022-12/DC solenoid valve
// - Omron G2R-1-S DC12(S) relay
//
// Features:
// - Automatic purge every 24 hours
// - Manual purge button with debounce
// - Startup functional test (1 second)
// - Non-blocking timing using millis()
// - Status LED indication
//     Slow blink = controller running normally
//     Fast blink = purge cycle active
// - Manual purge resets 24-hour timer
// - Startup test intentionally resets 24-hour timer
//
// Assumptions:
// - Relay input is active HIGH
// - Button uses INPUT_PULLUP
// - Button pressed = LOW
// - Relay output energizes purge solenoid
//
// Future Expansion:
// - Pressure transmitter feedback
// - Purge confirmation logic
// - Fault/alarm indication
// - Runtime statistics
// - PLC migration (Allen-Bradley Micro820)
//
// Revision History:
// v1.0.0 Initial release
// v1.0.5 Debounced manual purge button
// v1.1.0 Non-blocking state machine and status LED support
// v1.1.1 Ignore button during startup test; single-point 24-hour timer reset in startPurge()

// v1.4.0 MQTT purge and bounded duration configuration.
#include "purge_telemetry/UnoTelemetry.h"

const byte RELAY_PIN = 7;
const byte BUTTON_PIN = 8;
const byte LED_PIN = 13;

const unsigned long HOURS_TO_MS = 3600000UL;          // conversion factor: hours to milliseconds
const unsigned long INTERVAL_MS = 24UL * HOURS_TO_MS; // 24-hour interval between automatic purges
unsigned long activePurgeDurationMs = DEFAULT_PURGE_DURATION_MS; // latched at cycle start
const unsigned long DEBOUNCE_MS = 50UL;               // button debounce time
const unsigned long STARTUP_TEST_MS = 1000UL;         // startup functional test duration: 1 second

const unsigned long LED_NORMAL_INTERVAL = 1000UL;     // slow blink while running
const unsigned long LED_PURGE_INTERVAL = 125UL;       // fast blink while purging

unsigned long previousPurgeMillis = 0;
unsigned long lastButtonChangeMillis = 0;

// Startup test
bool startupTestActive = true;
unsigned long startupTestStartMillis = 0;

// Purge state
bool purgeActive = false;
unsigned long purgeStartMillis = 0;

// Button debounce
bool lastButtonReading = HIGH;
bool stableButtonState = HIGH;

// Status LED
bool ledState = LOW;
unsigned long lastLedToggleMillis = 0;

void setup()
{
    pinMode(RELAY_PIN, OUTPUT);          // RELAY CONTROL
    pinMode(BUTTON_PIN, INPUT_PULLUP);   // MANUAL BUTTON, PRESSED = LOW
    pinMode(LED_PIN, OUTPUT);            // STATUS LED

    // Ensure relay starts OFF
    digitalWrite(RELAY_PIN, LOW);

    // Startup functional test
    // NOTE:
    // This intentionally occurs on every power-up/reset
    // and resets the 24-hour purge schedule

    espLink.begin(9600);
    digitalWrite(RELAY_PIN, HIGH);

    startupTestActive = true;
    startupTestStartMillis = millis();
}

void loop()
{
    unsigned long currentMillis = millis();

    //--------------------------------------
    // Startup Test
    //--------------------------------------

    if (startupTestActive)
    {
        if (currentMillis - startupTestStartMillis >= STARTUP_TEST_MS)
        {
            digitalWrite(RELAY_PIN, LOW);

            startupTestActive = false;
            statusRequested = true;

            // Start 24-hour timer after startup test
            previousPurgeMillis = currentMillis;
        }
    }

    //--------------------------------------
    // Automatic Purge Timer
    //--------------------------------------

    if (!startupTestActive && !purgeActive)
    {
        if (currentMillis - previousPurgeMillis >= INTERVAL_MS)
        {
            startPurge(currentMillis);
        }
    }

    //--------------------------------------
    // Manual Button Handling with Debounce
    //--------------------------------------

    // Ignore the button until the startup test has finished, so a press
    // held at power-up cannot start a purge that the startup test then cuts short
    if (!startupTestActive)
    {
        bool reading = digitalRead(BUTTON_PIN);

        // Detect state change
        if (reading != lastButtonReading)
        {
            lastButtonChangeMillis = currentMillis;
            lastButtonReading = reading;
        }

        // If stable longer than debounce period
        if ((currentMillis - lastButtonChangeMillis) >= DEBOUNCE_MS)
        {
            if (reading != stableButtonState)
            {
                stableButtonState = reading;

                // Trigger once when button is pressed
                if (stableButtonState == LOW)
                {
                    startPurge(currentMillis);
                }
            }
        }
    }

    //--------------------------------------
    // Purge State Machine
    //--------------------------------------

    if (purgeActive)
    {
        if (currentMillis - purgeStartMillis >= activePurgeDurationMs)
        {
            digitalWrite(RELAY_PIN, LOW);

            purgeActive = false;
            recordPurgeComplete(currentMillis - purgeStartMillis);
        }
    }

    //--------------------------------------
    // Status LED
    //--------------------------------------

    unsigned long ledInterval =
        purgeActive ? LED_PURGE_INTERVAL : LED_NORMAL_INTERVAL;

    if (currentMillis - lastLedToggleMillis >= ledInterval)
    {
        lastLedToggleMillis = currentMillis;

        ledState = !ledState;

        digitalWrite(LED_PIN, ledState);
    }
    serviceTelemetry(millis(), startupTestActive, purgeActive,
                     startupTestActive ? startupTestStartMillis : purgeStartMillis,
                     startupTestActive ? STARTUP_TEST_MS : activePurgeDurationMs);
}

void startPurge(unsigned long currentMillis)
{
    // Apply local interlocks to every request source.
    if (startupTestActive || purgeActive)
    {
        return;
    }

    statusRequested = true;
    purgeActive = true;
    purgeStartMillis = currentMillis;
    activePurgeDurationMs = configuredPurgeDurationMs;

    // Any purge (automatic, manual or remote) restarts the 24-hour interval
    previousPurgeMillis = currentMillis;

    digitalWrite(RELAY_PIN, HIGH);
}
        
