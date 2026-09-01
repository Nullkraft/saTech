#pragma once

#include "command_interface.h"

void programAttenuatorRaw(uint8_t code);
void spiWrite32(ChipTarget target, uint32_t value);
