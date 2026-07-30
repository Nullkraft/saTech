#if defined(PIO_UNIT_TESTING)

#elif defined(ARDUINO)
#include <Arduino.h>
#include <arduino_hal.h>
#include <frequency_calculator.h>
#include <max2871.h>
#include <w25n_Flash.h>
#include "command_interface.h"
#include "console_state.h"
#include "technician_console.h"

// Metro Mini pinout (see MAX2871 examples/specAnn/specAnn.ino)
static constexpr double  REF_MHZ     = 66.0;
static constexpr double  STARTUP_RF_MHZ = 1735.113;

#ifndef SATECH_TECHNICIAN_CONSOLE
#define SATECH_TECHNICIAN_CONSOLE 0
#endif

ArduinoHAL halLo1(PIN_LE_LO1);
ArduinoHAL halLo2(PIN_LE_LO2);
ArduinoHAL halLo3(PIN_LE_LO3);

MAX2871 lo1(REF_MHZ, halLo1, halLo1);
MAX2871 lo2(REF_MHZ, halLo2, halLo2);
MAX2871 lo3(REF_MHZ, halLo3, halLo3);

FrequencyCalculator freqCalc(lo1, lo2, lo3);

double currentRfInputMhz = STARTUP_RF_MHZ;

W25N_Flash flash;

// cppcheck-suppress unusedFunction
void initializeLo(MAX2871& lo)
{
    lo.begin();
    lo.outputSelect(RF_B);
    lo.outputPower(+5, RF_B);
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

    freqCalc.FreqLO1 = lo1.fmn2freq();
    freqCalc.FreqLO2 = lo2.fmn2freq();
    freqCalc.FreqLO3 = lo3.fmn2freq();

    // Restore any LO that is under manual lofreq control; re-program the
    // hardware to the saved value so the auto-calculation does not overwrite it.
    if (s.lo1Manual) {
        lo1.setFrequency(savedLo1);
        freqCalc.FreqLO1 = savedLo1;
    }
    if (s.lo2Manual) {
        lo2.setFrequency(savedLo2);
        freqCalc.FreqLO2 = savedLo2;
    }
    if (s.lo3Manual) {
        lo3.setFrequency(savedLo3);
        freqCalc.FreqLO3 = savedLo3;
    }
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

void setup()
{
    Serial.begin(115200);
    const uint32_t serialStart = millis();
    while (!Serial && (millis() - serialStart) < 2000U) {
        // Wait briefly for USB serial on boards that need it.
    }

    // SPI.begin() is actually being called in ArduinoHAL::begin().
    // It enables the optional SPI functionality on the I/O pins.
    // Because ArduinoHAL is in another project folder SPI.begin()
    // is hidden from this project and no amount of searching will
    // reveal its true location. I duplicated it here to reduce
    // confusion.
    SPI.begin();
    resetConsoleState();
    saTech.begin(SerialEncoding::Ascii);

    freqCalc.RefClock1 = REF_MHZ;

    // HAL begin() sets the LE pins as OUTPUT.
    halLo1.begin();
    halLo2.begin();
    halLo3.begin();

    // Set remaining CS/LE and REF_EN pins as OUTPUT before driving them.
    pinMode(PIN_ATTEN,   OUTPUT);
    pinMode(PIN_REF_EN1, OUTPUT);
    pinMode(PIN_REF_EN2, OUTPUT);
    pinMode(PIN_ADC_1,    OUTPUT);
    pinMode(PIN_ADC_2,    OUTPUT);
    pinMode(PIN_RAM,     OUTPUT);
    pinMode(PIN_FLASH,   OUTPUT);
    pinMode(LED_BUILTIN, OUTPUT);
    // pinMode(PIN_LE_LO2,  OUTPUT);

    selectChip((ChipTarget::Flash));
    flash.begin(PIN_FLASH);
    // Deassert all CS/LE pins to idle and enable REF1 as the startup reference.
    selectChip(ChipTarget::Off);
    selectRef(ReferenceTarget::Ref1);

    initializeLo(lo1);
    initializeLo(lo2);
    initializeLo(lo3);

    Serial.println(F("Note: Tech Tool for board bring up. 3/26/26"));

    tuneTo(currentRfInputMhz);
#if SATECH_TECHNICIAN_CONSOLE
    printTechnicianBanner();
#endif
    printFulltestPlanReport();
}

void loop()
{
#if SATECH_TECHNICIAN_CONSOLE
    pollTechnicianConsole();
#else
    // See setup() where you can choose "ascii"  or "binary" communication
    pollSerial();
#endif
}

#else
int main()
{
    return 0;
}
#endif
