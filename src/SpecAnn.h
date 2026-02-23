#pragma once

#include <Arduino.h>
#include <arduino_hal.h>
#include <frequency_calculator.h>
#include <max2871.h>

struct SpecAnnPins {
    uint8_t status;
    uint8_t atten;
    uint8_t refEn1;
    uint8_t refEn2;
    uint8_t leLo1;
    uint8_t leLo2;
    uint8_t leLo3;
};

class SpecAnn {
public:
    SpecAnn(const SpecAnnPins& pins, double refMHz);

    void begin();
    void loop();

private:
    void initializeLo(MAX2871& lo);

    SpecAnnPins pins_;
    double refMHz_;
    ArduinoHAL halLo1_;
    ArduinoHAL halLo2_;
    ArduinoHAL halLo3_;
    MAX2871 lo1_;
    MAX2871 lo2_;
    MAX2871 lo3_;
    FrequencyCalculator freqCalc_;
};

