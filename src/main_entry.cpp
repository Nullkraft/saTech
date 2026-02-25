#if defined(PIO_UNIT_TESTING)

#elif defined(ARDUINO)
#include <Arduino.h>
#include <SPI.h>
#include <arduino_hal.h>
#include <frequency_calculator.h>
#include <max2871.h>
#include <math.h>
#include "command_interface.h"
#include "console_state.h"

// Metro Mini pinout (see MAX2871 examples/specAnn/specAnn.ino)
static constexpr uint8_t PIN_STATUS  = 10;
static constexpr double  REF_MHZ     = 66.0;
static constexpr double  STARTUP_RF_MHZ = 1735.113;
static constexpr uint16_t HEARTBEAT_INTERVAL_MS = 1;

ArduinoHAL halLo1(PIN_LE_LO1);
ArduinoHAL halLo2(PIN_LE_LO2);
ArduinoHAL halLo3(PIN_LE_LO3);

MAX2871 lo1(REF_MHZ, halLo1);
MAX2871 lo2(REF_MHZ, halLo2);
MAX2871 lo3(REF_MHZ, halLo3);

FrequencyCalculator freqCalc(lo1, lo2, lo3);

double currentRfInputMhz = STARTUP_RF_MHZ;
static uint32_t lastHeartbeatToggleMs = 0;
static bool heartbeatState = false;

// cppcheck-suppress unusedFunction
void initializeLo(MAX2871& lo)
{
#if !defined(SPECANN_CI_BUILD)
    lo.begin();
    lo.outputSelect(RF_ALL);
    lo.outputPower(+5, RF_ALL);
#else
    (void)lo;
#endif
}
static void printFrequencyPlan();
static void printLoSummary(const __FlashStringHelper* label, const MAX2871& lo);
void printStatus();
void tuneTo(double mhz);
void recomputePlan();
static void heartbeat();

void setup()
{
    pinMode(PIN_STATUS, OUTPUT);
    digitalWrite(PIN_STATUS, LOW);

    Serial.begin(115200);
    const uint32_t serialStart = millis();
    while (!Serial && (millis() - serialStart) < 2000U) {
        // Wait briefly for USB serial on boards that need it.
    }

    resetConsoleState();

    freqCalc.RefClock1 = REF_MHZ;

#if !defined(SPECANN_CI_BUILD)
    halLo1.begin();
    halLo2.begin();
    halLo3.begin();

    pinMode(PIN_ATTEN, OUTPUT);
    pinMode(PIN_REF_EN1, OUTPUT);
    pinMode(PIN_REF_EN2, OUTPUT);

    digitalWrite(PIN_ATTEN, HIGH); // Idle high so the attenuator is not latched unintentionally.
    digitalWrite(PIN_REF_EN1, HIGH);
    digitalWrite(PIN_REF_EN2, LOW);
    consoleState().ref1Enabled = true;
    consoleState().ref2Enabled = false;

    initializeLo(lo1);
    initializeLo(lo2);
    initializeLo(lo3);

    programAttenuatorDb(getCurrentAttenuatorDb());
    Serial.println(F("Note: Attenuator programming assumes PE43711 0.25 dB step codes—verify against hardware."));
#else
    Serial.println(F("SPECANN_CI_BUILD defined: hardware initialization skipped."));
#endif

    tuneTo(currentRfInputMhz);
    printBanner();
    printStatus();

    lastHeartbeatToggleMs = millis();
}

void loop()
{
    pollSerial();
    heartbeat();
}

void tuneTo(double mhz)
{
    currentRfInputMhz = mhz;
    // A fresh tune restores all LOs to automatic frequency control.
    ConsoleState& s = consoleState();
    s.lo1Manual = false;
    s.lo2Manual = false;
    s.lo3Manual = false;
    recomputePlan();
}

static void printFrequencyPlan()
{
    Serial.println(F("\nFrequency Plan"));
    Serial.print(F("RF In : ")); Serial.print(freqCalc.FreqRFin, 3); Serial.println(F(" MHz"));
    Serial.print(F("LO1  : ")); Serial.print(freqCalc.FreqLO1, 3);
    Serial.print(F(" MHz  IF1: ")); Serial.println(freqCalc.IF1, 3);
    Serial.print(F("LO2  : ")); Serial.print(freqCalc.FreqLO2, 3); Serial.println(F(" MHz"));
    Serial.print(F("LO3  : ")); Serial.print(freqCalc.FreqLO3, 3); Serial.println(F(" MHz"));
}

static void printLoSummary(const __FlashStringHelper* label, const MAX2871& lo)
{
    Serial.print(label);
    Serial.print(F(" M=")); Serial.print(lo.M);
    Serial.print(F(" F=")); Serial.print(lo.Frac);
    Serial.print(F(" N=")); Serial.print(lo.N);
    Serial.print(F(" DIVA=")); Serial.println(1 << lo.DIVA);
}

void printStatus()
{
    printFrequencyPlan();
    printLoSummary(F("LO1"), lo1);
    printLoSummary(F("LO2"), lo2);
    printLoSummary(F("LO3"), lo3);
    printInjectionSummary();
    Serial.print(F("Attenuator: ")); Serial.print(getCurrentAttenuatorDb(), 2); Serial.println(F(" dB"));
    Serial.print(F("Manual target: "));
    Serial.println(chipTargetName(getCurrentChipTarget()));
}

static void heartbeat()
{
    const uint32_t now = millis();
    if ((now - lastHeartbeatToggleMs) >= HEARTBEAT_INTERVAL_MS) {
        lastHeartbeatToggleMs = now;
        heartbeatState = !heartbeatState;
        digitalWrite(PIN_STATUS, heartbeatState ? HIGH : LOW);
    }
}

void recomputePlan()
{
    ConsoleState& s = consoleState();

    // Save manually-held LO frequencies before the auto-calculation overwrites them.
    const double savedLo1 = freqCalc.FreqLO1;
    const double savedLo2 = freqCalc.FreqLO2;
    const double savedLo3 = freqCalc.FreqLO3;

    // Overload B: pass the technician's desired injection modes so that
    // freqCalc.LO2InjectionMode / LO3InjectionMode are set correctly for display.
    freqCalc.set_LO_frequencies(currentRfInputMhz, freqCalc.RefClock1, 1,
                                 s.desiredLo2Injection,
                                 s.desiredLo3Injection);

#if !defined(SPECANN_CI_BUILD)
    freqCalc.FreqLO1 = lo1.fmn2freq();
    freqCalc.FreqLO2 = lo2.fmn2freq();
    freqCalc.FreqLO3 = lo3.fmn2freq();
#endif

    // Restore any LO that is under manual lofreq control; re-program the
    // hardware to the saved value so the auto-calculation does not overwrite it.
    if (s.lo1Manual) {
#if !defined(SPECANN_CI_BUILD)
        lo1.setFrequency(savedLo1);
#endif
        freqCalc.FreqLO1 = savedLo1;
    }
    if (s.lo2Manual) {
#if !defined(SPECANN_CI_BUILD)
        lo2.setFrequency(savedLo2);
#endif
        freqCalc.FreqLO2 = savedLo2;
    }
    if (s.lo3Manual) {
#if !defined(SPECANN_CI_BUILD)
        lo3.setFrequency(savedLo3);
#endif
        freqCalc.FreqLO3 = savedLo3;
    }
}

#else
int main()
{
    return 0;
}
#endif