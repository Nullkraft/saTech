#include <Arduino.h>
#include <unity.h>
#include "command_interface.h"
#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

static uint8_t deassertedLevel(uint8_t assertedLevel)
{
    return (assertedLevel == HIGH) ? LOW : HIGH;
}

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

void test_all_chip_select_pins_read_back_at_idle_levels(void)
{
    char message[96];

    // The special None case should leave every chip-select at its idle deasserted level.
    selectChip(ChipTarget::None);
    for (size_t observedIndex = 0; observedIndex < CHIP_SELECT_DEFINITION_COUNT; ++observedIndex) {
        const ChipSelectDefinition& observed = CHIP_SELECT_DEFINITIONS[observedIndex];
        const uint8_t expectedLevel = deassertedLevel(observed.assertedLevel);
        const uint8_t actualLevel = digitalRead(observed.pin);
        snprintf(
            message,
            sizeof(message),
            "selected=None observed=%s",
            chipTargetLabel(observed.target)
        );
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(expectedLevel, actualLevel, message);
    }
}

void test_all_chip_select_pins_read_back_at_asserted_levels(void)
{
    uint8_t assertHighPins[4] = {PIN_ATTEN, PIN_LE_LO1, PIN_LE_LO2, PIN_LE_LO3};
    uint8_t assertLowPins[4] = {PIN_ADC1, PIN_ADC2, PIN_RAM, PIN_FLASH};

    for (int i = 0; i < 4; i++) {
        digitalWrite(assertHighPins[i], HIGH);
        TEST_ASSERT_EQUAL(HIGH, digitalRead(assertHighPins[i]));
        digitalWrite(assertLowPins[i], LOW);
        TEST_ASSERT_EQUAL(LOW, digitalRead(assertLowPins[i]));
    }
}

void test_select_chip_asserts_selected_target_and_deasserts_the_rest(void)
{
    char message[96];

    // For each selectable target, verify that only that target is driven to
    // its asserted level and every other chip-select stays deasserted.
    for (size_t selectedIndex = 0; selectedIndex < CHIP_SELECT_DEFINITION_COUNT; ++selectedIndex) {
        const ChipSelectDefinition& selected = CHIP_SELECT_DEFINITIONS[selectedIndex];
        selectChip(selected.target);

        for (size_t observedIndex = 0; observedIndex < CHIP_SELECT_DEFINITION_COUNT; ++observedIndex) {
            const ChipSelectDefinition& observed = CHIP_SELECT_DEFINITIONS[observedIndex];
            const uint8_t expectedLevel =
                (observed.target == selected.target) ? observed.assertedLevel : deassertedLevel(observed.assertedLevel);
            const uint8_t actualLevel = digitalRead(observed.pin);
            snprintf(
                message,
                sizeof(message),
                "selected=%s observed=%s",
                chipTargetLabel(selected.target),
                chipTargetLabel(observed.target)
            );
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(expectedLevel, actualLevel, message);
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
    RUN_TEST(test_all_chip_select_pins_read_back_at_idle_levels);
    RUN_TEST(test_all_chip_select_pins_read_back_at_asserted_levels);
    RUN_TEST(test_select_chip_asserts_selected_target_and_deasserts_the_rest);
    UNITY_END();
}

void loop(void) {}
