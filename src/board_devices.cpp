#include "board_devices.h"

#include "command_interface.h"
#include "console_state.h"

#include <SPI.h>

namespace {

constexpr uint32_t Max2871SpiHz = 8000000UL;

class SaTechLoTransport : public I_MAX2871Transport {
public:
    SaTechLoTransport(ArduinoHAL& hal, ChipTarget target)
        : _hal(hal), _target(target) {}

    void spiWriteRegister(uint32_t value) override
    {
        SPI.beginTransaction(SPISettings(Max2871SpiHz, MSBFIRST, SPI_MODE0));
        selectChip(_target);
        _hal.spiWriteRegister(value);
        selectChip(ChipTarget::Off);
        SPI.endTransaction();
    }

    bool readMuxout() override
    {
        return _hal.readMuxout();
    }

private:
    ArduinoHAL& _hal;
    ChipTarget _target;
};

} // namespace

ArduinoHAL halLo1(PIN_LE_LO1);
ArduinoHAL halLo2(PIN_LE_LO2);
ArduinoHAL halLo3(PIN_LE_LO3);

SaTechLoTransport loTransport1(halLo1, ChipTarget::LO1);
SaTechLoTransport loTransport2(halLo2, ChipTarget::LO2);
SaTechLoTransport loTransport3(halLo3, ChipTarget::LO3);

MAX2871 lo1(REF_KHZ, loTransport1, halLo1);
MAX2871 lo2(REF_KHZ, loTransport2, halLo2);
MAX2871 lo3(REF_KHZ, loTransport3, halLo3);

FrequencyCalculator freqCalc(lo1, lo2, lo3);

IS66_Ram ram(PIN_RAM);
W25N_Flash flash;
uint8_t flashManufacturerId;
uint16_t flashDeviceId;
uint8_t flashProtection;
uint8_t flashConfiguration;

uint32_t currentRfInputKHz = STARTUP_RF_KHZ;

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
    const uint32_t savedLo1 = freqCalc.FreqLO1;
    const uint32_t savedLo2 = freqCalc.FreqLO2;
    const uint32_t savedLo3 = freqCalc.FreqLO3;

    // Overload B: pass the technician's desired injection modes so that
    // freqCalc.LO2InjectionMode / LO3InjectionMode are set correctly for display.
    freqCalc.set_LO_frequencies(currentRfInputKHz, freqCalc.RefClock1, 1,
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

void tuneTo(uint32_t khz)
{
    currentRfInputKHz = khz;
    // A fresh tune restores all LOs to automatic frequency control.
    ConsoleState& s = consoleState();
    s.lo1Manual = false;
    s.lo2Manual = false;
    s.lo3Manual = false;
    recomputePlan();
}
