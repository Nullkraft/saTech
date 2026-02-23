#if defined(PIO_UNIT_TESTING)

#elif defined(ARDUINO)
#include <Arduino.h>
#include "SpecAnn.h"

// Metro Mini pinout (see MAX2871 examples/specAnn/specAnn.ino)
static constexpr uint8_t PIN_ATTEN   = A5;
static constexpr uint8_t PIN_LE_LO1  = A3;
static constexpr uint8_t PIN_LE_LO2  = 4;
static constexpr uint8_t PIN_LE_LO3  = A4;
static constexpr uint8_t PIN_REF_EN1 = 5;
static constexpr uint8_t PIN_REF_EN2 = 6;
static constexpr uint8_t PIN_STATUS  = 10;
static constexpr double  REF_MHZ     = 66.0;

static const SpecAnnPins SPECANN_PINS = {
    PIN_STATUS,
    PIN_ATTEN,
    PIN_REF_EN1,
    PIN_REF_EN2,
    PIN_LE_LO1,
    PIN_LE_LO2,
    PIN_LE_LO3,
};

static SpecAnn specAnn(SPECANN_PINS, REF_MHZ);

void setup()
{
    specAnn.begin();
}

void loop()
{
    specAnn.loop();
}
#else
int main()
{
    return 0;
}
#endif
