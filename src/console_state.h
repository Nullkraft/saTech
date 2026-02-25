#pragma once

#include "command_interface.h"
#include <frequency_calculator.h>   // LOInjectionMode

struct ConsoleState {
    double attenuatorDb;
    ChipTarget chipTarget;
    bool manualSpiArmed;
    bool pendingSpiConfirmation;
    uint32_t pendingSpiValue;
    ChipTarget pendingSpiTarget;
    bool ref1Enabled;
    bool ref2Enabled;
    LOInjectionMode desiredLo2Injection;  // Persistent LO2 injection mode set by tech
    LOInjectionMode desiredLo3Injection;  // Persistent LO3 injection mode set by tech
    bool lo1Manual;  // true while LO1 frequency is under direct lofreq control
    bool lo2Manual;  // true while LO2 frequency is under direct lofreq control
    bool lo3Manual;  // true while LO3 frequency is under direct lofreq control
};

ConsoleState& consoleState();
void resetConsoleState();