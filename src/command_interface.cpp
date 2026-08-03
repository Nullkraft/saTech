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
    // Ensure the attenuator CS is idle before starting the SPI transaction.
    // programAttenuatorRaw() manages its own complete CS cycle independently
    // of selectChip(); the two mechanisms do not interfere with each other.
    digitalWrite(PIN_ATTEN, LOW);
    SPI.beginTransaction(SPISettings(ATTEN_SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_ATTEN, HIGH);
    SPI.transfer(code);
    digitalWrite(PIN_ATTEN, LOW);
    SPI.endTransaction();
    const double mappedDb = ATTEN_MIN_DB + (static_cast<double>(code) * ATTEN_STEP_DB);
    if (mappedDb >= ATTEN_MIN_DB && mappedDb <= (ATTEN_MAX_DB + 0.25)) {
        state.attenuatorDb = mappedDb;
    }
}

// Generic 32-bit SPI write for targets without a dedicated HAL object.
// assertLow: true if the CS pin asserts LOW (ADC, RAM, Flash);
//            false if it asserts HIGH.
// cppcheck-suppress unusedFunction
void spiWrite32(uint8_t csPin, bool assertLow, uint32_t value)
{
    const uint8_t assertLevel   = assertLow ? LOW  : HIGH;
    const uint8_t deassertLevel = assertLow ? HIGH : LOW;
    SPI.beginTransaction(SPISettings(SPI_DEFAULT_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(csPin, assertLevel);
    SPI.transfer(static_cast<uint8_t>((value >> 24) & 0xFFU));
    SPI.transfer(static_cast<uint8_t>((value >> 16) & 0xFFU));
    SPI.transfer(static_cast<uint8_t>((value >>  8) & 0xFFU));
    SPI.transfer(static_cast<uint8_t>( value        & 0xFFU));
    digitalWrite(csPin, deassertLevel);
    SPI.endTransaction();
}

// Shared chip-select metadata: target, assigned pin, and asserted raw level.
const ChipSelectDefinition CHIP_DEFINITIONS[] = {
    {ChipTarget::Attenuator, PIN_ATTEN, HIGH},
    {ChipTarget::LO1,        PIN_LE_LO1, HIGH},
    {ChipTarget::LO2,        PIN_LE_LO2, HIGH},
    {ChipTarget::LO3,        PIN_LE_LO3, HIGH},
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

    if (target == ChipTarget::Off) {
        Serial.println(F("All chip selects deasserted."));
    } else {
        Serial.print(F("Chip select set to "));
        Serial.println(chipTargetName(target));
    }
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
