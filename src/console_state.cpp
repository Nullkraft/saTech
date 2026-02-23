#include "console_state.h"

ConsoleState& consoleState()
{
    static ConsoleState state = {
        ATTEN_MIN_DB,
        ChipTarget::None,
        false,
        false,
        0U,
        ChipTarget::None,
        false,
        false
    };
    return state;
}

void resetConsoleState()
{
    ConsoleState& state = consoleState();
    state.attenuatorDb = ATTEN_MIN_DB;
    state.chipTarget = ChipTarget::None;
    state.manualSpiArmed = false;
    state.pendingSpiConfirmation = false;
    state.pendingSpiValue = 0U;
    state.pendingSpiTarget = ChipTarget::None;
    state.ref1Enabled = false;
    state.ref2Enabled = false;
}
