#pragma once

#include "command_interface.h"

void programAttenuatorRaw(uint8_t code);
void spiWrite32(uint8_t csPin, bool assertLow, uint32_t value);
