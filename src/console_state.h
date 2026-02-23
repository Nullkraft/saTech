#pragma once

#include "command_interface.h"

struct ConsoleState {
    double attenuatorDb;
    ChipTarget chipTarget;
    bool manualSpiArmed;
    bool pendingSpiConfirmation;
    uint32_t pendingSpiValue;
    ChipTarget pendingSpiTarget;
};

ConsoleState& consoleState();
void resetConsoleState();
