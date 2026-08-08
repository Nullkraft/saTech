#pragma once
#include <stdint.h>

#include "max2871_command_codes.h"
#include "w25n_command_codes.h"

const static uint8_t AddrAttenuator = 0x00U;
const static uint8_t AddrLo1 = 0x01U;
const static uint8_t AddrLo2 = 0x02U;
const static uint8_t AddrLo3 = 0x03U;
const static uint8_t AddrRefClocks = 0x04U;
const static uint8_t AddrAdc1 = 0x05U;
const static uint8_t AddrAdc2 = 0x06U;
const static uint8_t AddrRam = 0x07U;
const static uint8_t AddrFlash = 0x08U;
const static uint8_t AddrCommsState = 0x09U;
const static uint8_t AddrMessages = 0x0AU;

const static uint8_t CmdDigitalAttenuator = 0x01U;

const static uint8_t CmdRefOff = 0x00U;
const static uint8_t CmdRef1 = 0x01U;
const static uint8_t CmdRef2 = 0x02U;

const static uint8_t CmdCommandMode = 0x00U;
const static uint8_t CmdFmnMode = 0x01U;
const static uint8_t CmdDirectMode = 0x02U;

const static uint8_t CmdLedOff = 0x00U;
const static uint8_t CmdLedOn = 0x01U;
const static uint8_t CmdMessageRequest = 0x02U;
const static uint8_t CmdBeginSweep = 0x03U;
const static uint8_t CmdEndSweep = 0x04U;
const static uint8_t CmdResetHardwareReportPllStatus = 0x05U;
const static uint8_t CmdBeginMacro = 0x06U;
const static uint8_t CmdEndMacro = 0x07U;
const static uint8_t CmdSquelchLevel = 0x08U;

const static uint8_t CmdFlashId = 0x01U;
const static uint8_t CmdFlashRegisterReport = 0x02U;

const static uint16_t InstructionCommandFlag = 0x00FFU;
