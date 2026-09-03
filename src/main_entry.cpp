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

    SPI.begin();
    resetConsoleState();
    saTech.begin(SerialEncoding::Ascii);

    freqCalc.RefClock1 = REF_MHZ;

    // Initialize each LO HAL's SPI support.
    halLo1.begin();
    halLo2.begin();
    halLo3.begin();

    // Set CS/LE and REF_EN pins as OUTPUT before driving them.
    pinMode(PIN_LE_LO1,  OUTPUT);
    pinMode(PIN_LE_LO2,  OUTPUT);
    pinMode(PIN_LE_LO3,  OUTPUT);
    pinMode(PIN_ATTEN,   OUTPUT);
    pinMode(PIN_REF_EN1, OUTPUT);
    pinMode(PIN_REF_EN2, OUTPUT);
    pinMode(PIN_ADC_1,    OUTPUT);
    pinMode(PIN_ADC_2,    OUTPUT);
    pinMode(PIN_RAM,     OUTPUT);
    pinMode(PIN_FLASH,   OUTPUT);

    ram.begin();
    flash.begin();
    // Deassert all CS/LE pins to idle and enable REF1 as the startup reference.
    selectChip(ChipTarget::Off);

    SPI.beginTransaction(SPISettings(W25N_SPI_CLOCK_HZ, MSBFIRST, SPI_MODE0));
    selectChip(ChipTarget::Flash);
    flash.readJedecId(flashManufacturerId, flashDeviceId);
    selectChip(ChipTarget::Off);
    SPI.endTransaction();
    
    flash.readStatus(RegAddrProtect, &flashProtection);
    flash.readStatus(RegAddrConfigure, &flashConfiguration);

    uint8_t ramManufacturerId;
    uint16_t ramKgdId;
    SPI.beginTransaction(SPISettings(SPI_DEFAULT_HZ, MSBFIRST, SPI_MODE0));
    selectChip(ChipTarget::RAM);
    ram.readRamId(ramManufacturerId, ramKgdId);
    selectChip(ChipTarget::Off);
    SPI.endTransaction();

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
    // This path is used when operating with the technician console
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
