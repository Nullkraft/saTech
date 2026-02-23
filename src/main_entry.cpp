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
LOInjectionMode desiredLo1Injection = LOInjectionMode::High;
LOInjectionMode desiredLo2Injection = LOInjectionMode::High;
LOInjectionMode desiredLo3Injection = LOInjectionMode::High;

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
static const __FlashStringHelper* injectionModeName(LOInjectionMode mode);

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
    Serial.print(F(" DIVA=")); Serial.println(lo.DIVA);
}

void printStatus()
{
    printFrequencyPlan();
    printLoSummary(F("LO1"), lo1);
    printLoSummary(F("LO2"), lo2);
    printLoSummary(F("LO3"), lo3);
    Serial.print(F("Injection: LO1="));
    Serial.print(injectionModeName(freqCalc.LO1InjectionMode));
    Serial.print(F(" LO2="));
    Serial.print(injectionModeName(freqCalc.LO2InjectionMode));
    Serial.print(F(" LO3="));
    Serial.println(injectionModeName(freqCalc.LO3InjectionMode));
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

static const __FlashStringHelper* injectionModeName(LOInjectionMode mode)
{
    return (mode == LOInjectionMode::High) ? F("High") : F("Low");
}

void recomputePlan()
{
    freqCalc.FreqRFin = currentRfInputMhz;
    freqCalc.R = 1;

    const double fpfd = freqCalc.RefClock1 / static_cast<double>(freqCalc.R);
    const double if1Step = fpfd * round(freqCalc.IF1_center / fpfd);
    const int sign = (desiredLo1Injection == LOInjectionMode::High) ? 1 : -1;

    freqCalc.LO1InjectionMode = desiredLo1Injection;
    freqCalc.FreqLO1 = fpfd * round((if1Step + static_cast<double>(sign) * currentRfInputMhz) / fpfd);
    freqCalc.IF1 = freqCalc.FreqLO1 - (static_cast<double>(sign) * currentRfInputMhz);
    if (freqCalc.IF1 < 0.0) {
        freqCalc.IF1 = fabs(freqCalc.IF1);
    }

    freqCalc.LO2InjectionMode = desiredLo2Injection;
    if (desiredLo2Injection == LOInjectionMode::High) {
        freqCalc.FreqLO2 = freqCalc.IF1 + freqCalc.IF2;
    } else {
        freqCalc.FreqLO2 = freqCalc.IF1 - freqCalc.IF2;
    }
    if (freqCalc.FreqLO2 < 0.0) {
        Serial.println(F("WARNING: Computed LO2 frequency negative. Check injection selection."));
        freqCalc.FreqLO2 = fabs(freqCalc.FreqLO2);
    }

    freqCalc.LO3InjectionMode = desiredLo3Injection;
    if (desiredLo3Injection == LOInjectionMode::High) {
        freqCalc.FreqLO3 = freqCalc.IF2 + freqCalc.IF3;
    } else {
        freqCalc.FreqLO3 = freqCalc.IF2 - freqCalc.IF3;
    }
    if (freqCalc.FreqLO3 < 0.0) {
        Serial.println(F("WARNING: Computed LO3 frequency negative. Check injection selection."));
        freqCalc.FreqLO3 = fabs(freqCalc.FreqLO3);
    }

#if !defined(SPECANN_CI_BUILD)
    lo1.setFrequency(freqCalc.FreqLO1);
    lo2.setFrequency(freqCalc.FreqLO2);
    lo3.setFrequency(freqCalc.FreqLO3);
#endif

    freqCalc.FreqLO1 = lo1.fmn2freq();
    freqCalc.FreqLO2 = lo2.fmn2freq();
    freqCalc.FreqLO3 = lo3.fmn2freq();
}

#else
int main()
{
    return 0;
}
#endif
