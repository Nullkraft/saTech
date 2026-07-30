#include "board_devices.h"

#include "command_interface.h"
#include "console_state.h"

namespace {

constexpr double STARTUP_RF_MHZ = 1735.113;

} // namespace

ArduinoHAL halLo1(PIN_LE_LO1);
ArduinoHAL halLo2(PIN_LE_LO2);
ArduinoHAL halLo3(PIN_LE_LO3);

MAX2871 lo1(REF_MHZ, halLo1, halLo1);
MAX2871 lo2(REF_MHZ, halLo2, halLo2);
MAX2871 lo3(REF_MHZ, halLo3, halLo3);

FrequencyCalculator freqCalc(lo1, lo2, lo3);

W25N_Flash flash;

double currentRfInputMhz = STARTUP_RF_MHZ;

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
