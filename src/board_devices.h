#pragma once

#include <arduino_hal.h>
#include <frequency_calculator.h>
#include <max2871.h>
#include <w25n_Flash.h>

namespace {

constexpr double STARTUP_RF_MHZ = 1735.113;
constexpr uint32_t W25N_SPI_CLOCK_HZ = 16000000UL;

} // namespace

constexpr double REF_MHZ = 66.0;

// RF board
extern ArduinoHAL halLo1;
extern ArduinoHAL halLo2;
extern ArduinoHAL halLo3;
extern MAX2871 lo1;
extern MAX2871 lo2;
extern MAX2871 lo3;
extern FrequencyCalculator freqCalc;

// Controller board
extern W25N_Flash flash;
extern uint8_t flashManufacturerId;
extern uint16_t flashDeviceId;
extern uint8_t flashProtection;
extern uint8_t flashConfiguration;

extern double currentRfInputMhz;

void initializeLo(MAX2871& lo);
void recomputePlan();
void tuneTo(double mhz);
