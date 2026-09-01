#include "command_interface.h"

#include "board_control.h"
#include "console_state.h"

#include <SPI.h>
#include <math.h>

namespace {

ConsoleState& state = consoleState();

static inline double clampD(double v, double lo, double hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Enable during compile: Used by technician only when SATECH_TECHNICIAN_CONSOLE
uint8_t attenCodeFromDb(double db)
{
    const double clamped = clampD(db, ATTEN_MIN_DB, ATTEN_MAX_DB);
    const double steps = (clamped - ATTEN_MIN_DB) / ATTEN_STEP_DB;
    return static_cast<uint8_t>(lround(steps));
}

} // namespace

void programAttenuatorRaw(uint8_t code)
{
    SPI.beginTransaction(SPISettings(ATTEN_SPI_HZ, MSBFIRST, SPI_MODE0));
    selectChip(ChipTarget::Attenuator);
    SPI.transfer(code);
    selectChip(ChipTarget::Off);
    SPI.endTransaction();
    selectChip(ChipTarget::Off);
    const double mappedDb = ATTEN_MIN_DB + (static_cast<double>(code) * ATTEN_STEP_DB);
    if (mappedDb >= ATTEN_MIN_DB && mappedDb <= (ATTEN_MAX_DB + 0.25)) {
        state.attenuatorDb = mappedDb;
    }
}

// Generic 32-bit SPI write for targets without a dedicated HAL object.
// cppcheck-suppress unusedFunction
void spiWrite32(ChipTarget target, uint32_t value)
{
    SPI.beginTransaction(SPISettings(SPI_DEFAULT_HZ, MSBFIRST, SPI_MODE0));
    selectChip(target);
    SPI.transfer(static_cast<uint8_t>((value >> 24) & 0xFFU));
    SPI.transfer(static_cast<uint8_t>((value >> 16) & 0xFFU));
    SPI.transfer(static_cast<uint8_t>((value >>  8) & 0xFFU));
    SPI.transfer(static_cast<uint8_t>( value        & 0xFFU));
    selectChip(ChipTarget::Off);
    SPI.endTransaction();
}

// Shared chip-select metadata: target, assigned pin, and asserted raw level.
const ChipSelectDefinition CHIP_DEFINITIONS[] = {
    {ChipTarget::Attenuator, PIN_ATTEN, HIGH},
    {ChipTarget::LO1,        PIN_LE_LO1, LOW},
    {ChipTarget::LO2,        PIN_LE_LO2, LOW},
    {ChipTarget::LO3,        PIN_LE_LO3, LOW},
    {ChipTarget::RAM,        PIN_RAM,    LOW},
    {ChipTarget::Flash,      PIN_FLASH,  LOW},
    {ChipTarget::ADC_1,       PIN_ADC_1,   LOW},
    {ChipTarget::ADC_2,       PIN_ADC_2,   LOW},
};

const size_t CHIP_COUNT =
    sizeof(CHIP_DEFINITIONS) / sizeof(CHIP_DEFINITIONS[0]);

const ChipSelectDefinition* chipSelectDefinitionForTarget(ChipTarget target)
{
    for (size_t i = 0; i < CHIP_COUNT; ++i) {
        if (CHIP_DEFINITIONS[i].chip == target) {
            return &CHIP_DEFINITIONS[i];
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// selectChip — public low-level primitive.
//
// Deasserts all CS/LE pins to their idle levels, then asserts the requested
// target's pin. ChipTarget::Off deasserts everything and leaves it that way.
// Resets the SPI arming state on every call so a chip switch cannot carry over
// a previously armed write.
//
// This function owns state.chipTarget and is the only site that writes it.
// It is intentionally side-effect-free beyond pin state and state.chipTarget.
// ---------------------------------------------------------------------------
void selectChip(ChipTarget target)
{
    for (size_t i = 0; i < CHIP_COUNT; ++i) {
        const ChipSelectDefinition& def = CHIP_DEFINITIONS[i];
        digitalWrite(def.pin, (def.assertedLevel == HIGH) ? LOW : HIGH);
    }

    const ChipSelectDefinition* selected = chipSelectDefinitionForTarget(target);
    if (selected != nullptr) {
        digitalWrite(selected->pin, selected->assertedLevel);
    }

    ConsoleState& s = consoleState();
    s.chipTarget            = target;
    s.manualSpiArmed        = false;
    s.pendingSpiConfirmation = false;

}

// ---------------------------------------------------------------------------
// selectRef — public low-level primitive.
//
// Deasserts both REF_EN pins, then asserts the requested reference clock.
// ReferenceTarget::Off disables both clocks.
// Any target value other than Ref1, Ref2, or Off leaves both clocks disabled.
//
// This function owns state.ref1Enabled and state.ref2Enabled and is the only
// site that writes those flags.
// It is intentionally side-effect-free beyond pin state and those two flags.
// ---------------------------------------------------------------------------
void selectRef(ReferenceTarget target)
{
    ConsoleState& s = consoleState();

    // Deassert both reference clocks unconditionally.
    digitalWrite(PIN_REF_EN1, LOW);
    digitalWrite(PIN_REF_EN2, LOW);
    s.ref1Enabled = false;
    s.ref2Enabled = false;

    switch (target) {
    case ReferenceTarget::Ref1:
        digitalWrite(PIN_REF_EN1, HIGH);
        s.ref1Enabled = true;
        break;
    case ReferenceTarget::Ref2:
        digitalWrite(PIN_REF_EN2, HIGH);
        s.ref2Enabled = true;
        break;
    default:
        // ReferenceTarget::Off - both clocks remain deasserted.
        break;
    }
}

// cppcheck-suppress unusedFunction
void programAttenuatorDb(double db)
{
    const uint8_t code = attenCodeFromDb(db);
    programAttenuatorRaw(code);
    state.attenuatorDb = ATTEN_MIN_DB + (static_cast<double>(code) * ATTEN_STEP_DB);
}

// cppcheck-suppress unusedFunction
double getCurrentAttenuatorDb()
{
    return state.attenuatorDb;
}

// cppcheck-suppress unusedFunction
ChipTarget getCurrentChipTarget()
{
    return state.chipTarget;
}

const __FlashStringHelper* chipTargetName(ChipTarget target)
{
    switch (target) {
        case ChipTarget::LO1:        return F("LO1");
        case ChipTarget::LO2:        return F("LO2");
        case ChipTarget::LO3:        return F("LO3");
        case ChipTarget::Attenuator: return F("Attenuator");
        case ChipTarget::ADC_1:       return F("ADC_1");
        case ChipTarget::ADC_2:       return F("ADC_2");
        case ChipTarget::RAM:        return F("RAM");
        case ChipTarget::Flash:      return F("FLASH");
        case ChipTarget::Off:        return F("Off");
        default:                     return F("None");
    }
}
