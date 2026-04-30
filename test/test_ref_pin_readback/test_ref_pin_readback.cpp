#include <Arduino.h>
#include <unity.h>

#include "command_interface.h"

void setUp(void) {}
void tearDown(void) {}

void test_ref_off_deasserts_reference_enable_outputs(void)
{
    pinMode(PIN_REF_EN1, OUTPUT);
    pinMode(PIN_REF_EN2, OUTPUT);

    selectRef(ReferenceTarget::Ref1);
    selectRef(ReferenceTarget::None);

    TEST_ASSERT_EQUAL_INT(LOW, readOutputPinLevel(PIN_REF_EN1));
    TEST_ASSERT_EQUAL_INT(LOW, readOutputPinLevel(PIN_REF_EN2));
}

void test_ref1_on_asserts_reference_enable_outputs(void)
{
    pinMode(PIN_REF_EN1, OUTPUT);
    pinMode(PIN_REF_EN2, OUTPUT);

    selectRef(ReferenceTarget::Ref1);

    TEST_ASSERT_EQUAL_INT(HIGH, readOutputPinLevel(PIN_REF_EN1));
    TEST_ASSERT_EQUAL_INT(LOW, readOutputPinLevel(PIN_REF_EN2));
}

void test_ref2_on_asserts_reference_enable_outputs(void)
{
    pinMode(PIN_REF_EN1, OUTPUT);
    pinMode(PIN_REF_EN2, OUTPUT);

    selectRef(ReferenceTarget::Ref2);

    TEST_ASSERT_EQUAL_INT(LOW, readOutputPinLevel(PIN_REF_EN1));
    TEST_ASSERT_EQUAL_INT(HIGH, readOutputPinLevel(PIN_REF_EN2));
}

void setup(void)
{
    Serial.begin(115200);

    UNITY_BEGIN();
    RUN_TEST(test_ref_off_deasserts_reference_enable_outputs);
    RUN_TEST(test_ref1_on_asserts_reference_enable_outputs);
    RUN_TEST(test_ref2_on_asserts_reference_enable_outputs);
    UNITY_END();
}

void loop(void) {}
