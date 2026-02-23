#pragma once

#include "command_interface.h"

struct ConsoleState {
    double attenuatorDb;
    ChipTarget chipTarget;
    bool manualSpiArmed;
    bool pendingSpiConfirmation;
    uint32_t pendingSpiValue;
    ChipTarget pendingSpiTarget;
    bool ref1Enabled;
    bool ref2Enabled;
};

ConsoleState& consoleState();
void resetConsoleState();
