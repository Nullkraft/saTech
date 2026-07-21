#pragma once

#include <Arduino.h>
#include <stdint.h>

// LE / chip-select pins — LO1, LO2, LO3, and attenuator assert HIGH
constexpr uint8_t PIN_ATTEN   = A5;
constexpr uint8_t PIN_LE_LO1  = A3;
constexpr uint8_t PIN_LE_LO2  = 4;
constexpr uint8_t PIN_LE_LO3  = A4;

// LE / chip-select pins — ADC_1, ADC_2, RAM, and Flash assert LOW
constexpr uint8_t PIN_ADC_1    = 3;
constexpr uint8_t PIN_ADC_2    = 2;
constexpr uint8_t PIN_RAM     = A0;
constexpr uint8_t PIN_FLASH   = A1;

// Reference clock enable pins — both assert HIGH.
constexpr uint8_t PIN_REF_EN1 = 5;
constexpr uint8_t PIN_REF_EN2 = 6;

enum class ChipTarget { None, LO1, LO2, LO3, Attenuator, ADC_1, ADC_2, RAM, Flash };
enum class ReferenceTarget { None, Ref1, Ref2 };
enum class SerialTransportEncoding { Ascii, Binary };

class SaTech {
public:
    bool begin(const char* encoding);
    bool supportsEncoding(const char* encoding) const;
    SerialTransportEncoding transportEncoding() const;
};

extern SaTech saTech;

struct ChipSelectDefinition {
    ChipTarget chip;
    uint8_t pin;
    uint8_t assertedLevel;
};

extern const ChipSelectDefinition CHIP_DEFINITIONS[];
extern const size_t CHIP_COUNT;

constexpr double MIN_RF_INPUT_MHZ = 0.001;
constexpr double MAX_RF_INPUT_MHZ = 6000.0;
constexpr double ATTEN_MIN_DB     = 0.0;
constexpr double ATTEN_MAX_DB     = 31.75;
constexpr double ATTEN_STEP_DB    = 0.25;
constexpr uint32_t ATTEN_SPI_HZ   = 1000000UL;
constexpr uint32_t SPI_DEFAULT_HZ = 16000000UL;  // Default for ADC, RAM, Flash raw writes.
constexpr size_t INPUT_BUFFER_SIZE = 96;

// ---------------------------------------------------------------------------
// Low-level pin primitives — reusable across technician, calibration, and
// normal modes without going through the serial console layer.
//
// selectChip() and selectRef() are intentionally side-effect-free beyond pin
// state and the two state flags they own (chipTarget, ref1/ref2Enabled).
//
// NOTE: state.chipTarget and the HAL LE pin toggling are intentionally
// independent mechanisms. selectChip() is for manual technician pin control.
// halLo1/halLo2/halLo3 manage their own LE lines autonomously during
// programmatic SPI writes (e.g. tuneTo(), recomputePlan()). The two paths
// do not interfere with each other.
// ---------------------------------------------------------------------------

// Deasserts all CS/LE pins, then asserts the requested target's pin.
// ChipTarget::None deasserts everything and leaves it that way.
// Resets the SPI arming state on every call.
void selectChip(ChipTarget target);
void selectSerialChipTarget(ChipTarget target);

// Deasserts both REF_EN pins, then asserts the requested reference clock.
// ReferenceTarget::Ref1 or Ref2 selects that clock; ReferenceTarget::None
// disables both clocks.
void selectRef(ReferenceTarget target);

void pollSerial();
void processReceivedWord(uint32_t word);
void processDirectRegisterData(uint32_t value);

void programAttenuatorDb(double db);
double getCurrentAttenuatorDb();
ChipTarget getCurrentChipTarget();
const __FlashStringHelper* chipTargetName(ChipTarget target);
