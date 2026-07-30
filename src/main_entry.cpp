#if defined(PIO_UNIT_TESTING)

#elif defined(ARDUINO)
#include <Arduino.h>
#include <SPI.h>
#include "board_devices.h"
#include "command_interface.h"
#include "console_state.h"
#include "technician_console.h"

#ifndef SATECH_TECHNICIAN_CONSOLE
#define SATECH_TECHNICIAN_CONSOLE 0
#endif

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

    selectChip(ChipTarget::Flash);
    flash.begin();
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
