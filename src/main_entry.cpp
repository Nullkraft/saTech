#if defined(PIO_UNIT_TESTING)

#elif defined(ARDUINO)
#include <Arduino.h>
#include <arduino_hal.h>
#include <frequency_calculator.h>
#include <max2871.h>

// Metro Mini pinout (see MAX2871 examples/specAnn/specAnn.ino)
static constexpr uint8_t PIN_ATTEN   = A5;
static constexpr uint8_t PIN_LE_LO1  = A3;
static constexpr uint8_t PIN_LE_LO2  = 4;
static constexpr uint8_t PIN_LE_LO3  = A4;
static constexpr uint8_t PIN_REF_EN1 = 5;
static constexpr uint8_t PIN_REF_EN2 = 6;
static constexpr uint8_t PIN_STATUS  = 10;
static constexpr double  REF_MHZ     = 66.0;

static ArduinoHAL halLo1(PIN_LE_LO1);
static ArduinoHAL halLo2(PIN_LE_LO2);
static ArduinoHAL halLo3(PIN_LE_LO3);

static MAX2871 lo1(REF_MHZ, halLo1);
static MAX2871 lo2(REF_MHZ, halLo2);
static MAX2871 lo3(REF_MHZ, halLo3);

static FrequencyCalculator freqCalc(lo1, lo2, lo3);

static void initializeLo(MAX2871& lo)
{
    lo.begin();
    lo.outputSelect(RF_ALL);
    lo.outputPower(+5, RF_ALL);
}

void setup()
{
    pinMode(PIN_STATUS, OUTPUT);
    digitalWrite(PIN_STATUS, LOW);

    Serial.begin(115200);

    halLo1.begin();
    halLo2.begin();
    halLo3.begin();

    pinMode(PIN_ATTEN, OUTPUT);
    pinMode(PIN_REF_EN1, OUTPUT);
    pinMode(PIN_REF_EN2, OUTPUT);

    digitalWrite(PIN_ATTEN, LOW);
    digitalWrite(PIN_REF_EN1, HIGH);
    digitalWrite(PIN_REF_EN2, LOW);

    initializeLo(lo1);
    initializeLo(lo2);
    initializeLo(lo3);

    freqCalc.set_LO_frequencies(1735.113, freqCalc.RefClock1, 1);

    Serial.println(F("SpecAnn startup LO plan:"));
    Serial.print(F("  LO1 = "));
    Serial.print(freqCalc.FreqLO1, 3);
    Serial.println(F(" MHz"));
    Serial.print(F("  LO2 = "));
    Serial.print(freqCalc.FreqLO2, 3);
    Serial.println(F(" MHz"));
    Serial.print(F("  LO3 = "));
    Serial.print(freqCalc.FreqLO3, 3);
    Serial.println(F(" MHz"));
}

void loop()
{
    digitalWrite(PIN_STATUS, HIGH);
    delay(1);
    digitalWrite(PIN_STATUS, LOW);
    delay(1);
}
#else
int main()
{
    return 0;
}
#endif
