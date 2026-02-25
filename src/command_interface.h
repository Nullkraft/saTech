#pragma once

#include <Arduino.h>
#include <stdint.h>

constexpr uint8_t PIN_ATTEN   = A5;
constexpr uint8_t PIN_LE_LO1  = A3;
constexpr uint8_t PIN_LE_LO2  = 4;
constexpr uint8_t PIN_LE_LO3  = A4;
constexpr uint8_t PIN_REF_EN1 = 5;
constexpr uint8_t PIN_REF_EN2 = 6;

enum class ChipTarget { None, LO1, LO2, LO3, Attenuator, Ref1, Ref2, ADC1, ADC2, RAM, Flash };

constexpr double MIN_RF_INPUT_MHZ = 23.5;
constexpr double MAX_RF_INPUT_MHZ = 6000.0;
constexpr double ATTEN_MIN_DB = 1.0;
constexpr double ATTEN_MAX_DB = 31.75;
constexpr double ATTEN_STEP_DB = 0.25;
constexpr uint32_t ATTEN_SPI_HZ = 1000000UL;
constexpr size_t INPUT_BUFFER_SIZE = 96;

void printBanner();
void pollSerial();
void printInjectionSummary();

void programAttenuatorDb(double db);
double getCurrentAttenuatorDb();
ChipTarget getCurrentChipTarget();
const __FlashStringHelper* chipTargetName(ChipTarget target);