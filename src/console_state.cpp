#include "console_state.h"

ConsoleState& consoleState()
{
    static ConsoleState state = {
        ATTEN_MIN_DB,           // attenuatorDb
        ChipTarget::None,       // chipTarget
        false,                  // manualSpiArmed
        false,                  // pendingSpiConfirmation
        0U,                     // pendingSpiValue
        ChipTarget::None,       // pendingSpiTarget
        false,                  // ref1Enabled
        false,                  // ref2Enabled
        LOInjectionMode::High,  // desiredLo2Injection
        LOInjectionMode::High,  // desiredLo3Injection
        false,                  // lo1Manual
        false,                  // lo2Manual
        false                   // lo3Manual
    };
    return state;
}

// cppcheck-suppress unusedFunction
void resetConsoleState()
{
    ConsoleState& state = consoleState();
    state.attenuatorDb          = ATTEN_MIN_DB;
    state.chipTarget            = ChipTarget::None;
    state.manualSpiArmed        = false;
    state.pendingSpiConfirmation = false;
    state.pendingSpiValue       = 0U;
    state.pendingSpiTarget      = ChipTarget::None;
    state.ref1Enabled           = false;
    state.ref2Enabled           = false;
    state.desiredLo2Injection   = LOInjectionMode::High;
    state.desiredLo3Injection   = LOInjectionMode::High;
    state.lo1Manual             = false;
    state.lo2Manual             = false;
    state.lo3Manual             = false;
}