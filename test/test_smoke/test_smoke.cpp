#include <Arduino.h>
#include <unity.h>
#include "command_interface.h"
#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

static const char* chipTargetLabel(ChipTarget target)
{
    switch (target) {
        case ChipTarget::Attenuator: return "Attenuator";
        case ChipTarget::LO1:        return "LO1";
        case ChipTarget::LO2:        return "LO2";
        case ChipTarget::LO3:        return "LO3";
        case ChipTarget::ADC1:       return "ADC1";
        case ChipTarget::ADC2:       return "ADC2";
        case ChipTarget::RAM:        return "RAM";
        case ChipTarget::Flash:      return "Flash";
        case ChipTarget::None:       return "None";
        default:                     return "Unknown";
    }
}

void test_all_chip_select_pins_are_deasserted(void)
{
    char message[96];

    // The special None case should leave every chip-select at its idle deasserted level.
    selectChip(ChipTarget::None);
    for (size_t observedIdx = 0; observedIdx < CHIP_COUNT; ++observedIdx) {
        const ChipSelectDefinition& observed = CHIP_DEFINITIONS[observedIdx];
        const uint8_t deassertedLevel = !observed.assertedLevel;
        const uint8_t actualLevel = digitalRead(observed.pin);
        snprintf(
            message,
            sizeof(message),
            "selected=None observed=%s",
            chipTargetLabel(observed.target)
        );
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(actualLevel, deassertedLevel, message);
    }
}

void test_each_chip_select_pin_is_asserted(void)
{
    char message[96];

    // Verify that only the target's chip-select is driven to its asserted level
    // and that every other chip-select stays deasserted.
    for (size_t selectedIndex = 0; selectedIndex < CHIP_COUNT; ++selectedIndex) {
        const ChipSelectDefinition& selected = CHIP_DEFINITIONS[selectedIndex];
        selectChip(selected.target);

        for (size_t observedIdx = 0; observedIdx < CHIP_COUNT; ++observedIdx) {
            const ChipSelectDefinition& observed = CHIP_DEFINITIONS[observedIdx];
            const uint8_t expectedLevel =
                (observed.target == selected.target) ? observed.assertedLevel : !observed.assertedLevel;
            const uint8_t actualLevel = digitalRead(observed.pin);
            snprintf(
                message,
                sizeof(message),
                "selected=%s observed=%s",
                chipTargetLabel(selected.target),
                chipTargetLabel(observed.target)
            );
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(actualLevel, expectedLevel, message);
        }
    }
}

void setup(void)
{
    Serial.begin(115200);

    pinMode(PIN_ATTEN, OUTPUT);
    pinMode(PIN_LE_LO1, OUTPUT);
    pinMode(PIN_LE_LO2, OUTPUT);
    pinMode(PIN_LE_LO3, OUTPUT);
    pinMode(PIN_ADC1, OUTPUT);
    pinMode(PIN_ADC2, OUTPUT);
    pinMode(PIN_RAM, OUTPUT);
    pinMode(PIN_FLASH, OUTPUT);

    UNITY_BEGIN();
    RUN_TEST(test_all_chip_select_pins_are_deasserted);
    RUN_TEST(test_each_chip_select_pin_is_asserted);
    UNITY_END();
}

void loop(void) {}
