#include "command_interface.h"

#include "binary_command_executor.h"
#include "board_devices.h"
#include "board_control.h"
#include "console_state.h"
#include "technician_console.h"
#include "command_Codes.h"

#include <SPI.h>

namespace {

constexpr uint8_t RECEIVED_WORD_BYTES = 4U;
ConsoleState& state = consoleState();
MAX2871* LO;    // Global object for currently selected LO

constexpr uint16_t instructionWord(uint8_t commandBits, uint8_t chipAddress)
{
    return (static_cast<uint16_t>(commandBits) << 12) |
           (static_cast<uint16_t>(chipAddress) << 8) |
           InstructionCommandFlag;
}

constexpr uint32_t SerialAsciiWord = 0x000106FFUL;
constexpr uint32_t SerialBinaryWord = 0x000206FFUL;

// Only Command mode supports 0xFF, FMNData and DirectRegisterData are guaranteed to never have 0xFF in their LSB's
enum class SerialPayloadMode {
    Command,                // A command is recognized when the 8 LSB's are 0xFF
    FMNData,                // F, M, and N divider values for programming MAX2871 LO's
    DirectRegisterData,     // Directly program MAX2871 registers when this mode is set
};

struct SerialReceiveState {
    SerialPayloadMode payloadMode;
    uint8_t wordBytes[RECEIVED_WORD_BYTES];
    uint8_t wordLength;
};

enum class LoCommand {
    None,
    DisableOutput,
    RfBPowerMinus4,
    RfBPowerMinus1,
    RfBPowerPlus2,
    RfBPowerPlus5,
    FmnData,
    Ignored,
};

SerialReceiveState serialRxState = {
    SerialPayloadMode::Command,
    {0U, 0U, 0U, 0U},
    0U,
};

bool setChipTarget(uint16_t commandCode, ChipTarget* target)
{
    if (commandCode == instructionWord(CmdGeneral, AddrLo1)) {
        *target = ChipTarget::LO1;
        return true;
    }
    if (commandCode == instructionWord(CmdGeneral, AddrLo2)) {
        *target = ChipTarget::LO2;
        return true;
    }
    if (commandCode == instructionWord(CmdGeneral, AddrLo3)) {
        *target = ChipTarget::LO3;
        return true;
    }
    return false;   // Handles command codes that are not LO control codes
}

ChipTarget getChipTarget(uint8_t chipAddress)
{
    if (chipAddress == AddrLo1) {
        LO = &lo1;
        return ChipTarget::LO1;
    }
    if (chipAddress == AddrLo2) {
        LO = &lo2;
        return ChipTarget::LO2;
    }
    if (chipAddress == AddrLo3) {
        LO = &lo3;
        return ChipTarget::LO3;
    }
    return ChipTarget::None;
}

LoCommand getLoCommand(uint8_t commandBits)
{
    if (commandBits == CmdRfOff) return LoCommand::DisableOutput;
    if (commandBits == CmdRfPowerMinus4) return LoCommand::RfBPowerMinus4;
    if (commandBits == CmdRfPowerMinus1) return LoCommand::RfBPowerMinus1;
    if (commandBits == CmdRfPowerPlus2) return LoCommand::RfBPowerPlus2;
    if (commandBits == CmdRfPowerPlus5) return LoCommand::RfBPowerPlus5;
    if (commandBits == CmdSetFrequency) return LoCommand::FmnData;
    if (commandBits == CmdMuxTriState || commandBits == CmdMuxDigitalLockDetect ||
        commandBits == CmdDivaMode) return LoCommand::Ignored;
    return LoCommand::None;
}

bool decodeLoCommand(uint16_t commandCode, LoCommand* loCommand)
{
    if ((commandCode & InstructionCommandFlag) != InstructionCommandFlag) {
        return false;
    }
    const uint8_t commandByte = static_cast<uint8_t>((commandCode >> 8) & 0xFFU);
    const uint8_t chipAddress = commandByte & 0x0FU;
    const uint8_t commandBits = commandByte >> 4;

    const ChipTarget target = getChipTarget(chipAddress);
    if (target == ChipTarget::None) {
        return false;
    }

    const LoCommand command = getLoCommand(commandBits);
    if (command == LoCommand::None) {
        return false;
    }

    selectChip(target);
    *loCommand = command;
    return true;
}

void setSerialPayloadMode(SerialPayloadMode mode)
{
    serialRxState.payloadMode = mode;
    serialRxState.wordLength = 0U;
}

void deassertAllChipSelectPins()
{
    for (size_t i = 0; i < CHIP_COUNT; ++i) {
        const ChipSelectDefinition& def = CHIP_DEFINITIONS[i];
        digitalWrite(def.pin, (def.assertedLevel == HIGH) ? LOW : HIGH);
    }
    state.chipTarget = ChipTarget::Off;
    state.manualSpiArmed = false;
    state.pendingSpiConfirmation = false;
}

void handleControlWord(uint32_t word)
{
    const uint16_t commandCode = static_cast<uint16_t>(word & 0xFFFFU);
    if (serialRxState.payloadMode == SerialPayloadMode::Command) {
        if (word == SerialAsciiWord) {
            setSerialEncoding(SerialEncoding::Ascii);
            return;
        }
        if (word == SerialBinaryWord) {
            setSerialEncoding(SerialEncoding::Binary);
            return;
        }
    }
    if (commandCode == instructionWord(CmdLedOn, AddrMessages)) {
        digitalWrite(LED_BUILTIN, HIGH);
        Serial.print(F("LED on"));
        return;
    }
    if (commandCode == instructionWord(CmdLedOff, AddrMessages)) {
        digitalWrite(LED_BUILTIN, LOW);
        Serial.print(F("LED off"));
        return;
    }
    if (commandCode == instructionWord(CmdMessageRequest, AddrMessages)) {
        Serial.print(F("saTech WN2A ready"));
        return;
    }
    // Unimplemented - Begin/End Macro, Begin/End Sweep, and Squelch Level
    if (commandCode == instructionWord(CmdBeginSweep, AddrMessages) ||
        commandCode == instructionWord(CmdEndSweep, AddrMessages) ||
        commandCode == instructionWord(CmdBeginMacro, AddrMessages) ||
        commandCode == instructionWord(CmdEndMacro, AddrMessages) ||
        commandCode == instructionWord(CmdSquelchLevel, AddrMessages)) {
        return;
    }
    if (commandCode == instructionWord(CmdResetHardwareReportPllStatus, AddrMessages)) {
        initializeLo(lo1);
        initializeLo(lo2);
        initializeLo(lo3);
        tuneTo(currentRfInputMhz);
        printFulltestPlanReport();
        return;
    }
    if (commandCode == instructionWord(CmdCommandMode, AddrCommsState)) {
        setSerialPayloadMode(SerialPayloadMode::Command);
        return;
    }
    if (commandCode == instructionWord(CmdFmnMode, AddrCommsState)) {
        setSerialPayloadMode(SerialPayloadMode::FMNData);
        return;
    }
    if (commandCode == instructionWord(CmdDirectMode, AddrCommsState)) {
        setSerialPayloadMode(SerialPayloadMode::DirectRegisterData);
        return;
    }
    if (commandCode == instructionWord(CmdRefOff, AddrRefClocks)) {
        selectRef(ReferenceTarget::Off);
        return;
    }
    if (commandCode == instructionWord(CmdRef1, AddrRefClocks)) {
        selectRef(ReferenceTarget::Ref1);
        return;
    }
    if (commandCode == instructionWord(CmdRef2, AddrRefClocks)) {
        selectRef(ReferenceTarget::Ref2);
        return;
    }
    ChipTarget loTarget;
    if (setChipTarget(commandCode, &loTarget)) {
        selectChip(loTarget);
        return;
    }
    if (commandCode == instructionWord(CmdDigitalAttenuator, AddrAttenuator)) {
        programAttenuatorRaw(static_cast<uint8_t>((word >> 16) & 0x7FU));
        deassertAllChipSelectPins();
        return;
    }
    LoCommand loCommand;
    if (decodeLoCommand(commandCode, &loCommand)) {
        if (loCommand == LoCommand::DisableOutput) {
            LO->outputSelect(RFNONE);
            deassertAllChipSelectPins();
        } else if (loCommand == LoCommand::RfBPowerMinus4) {
            LO->outputPower(-4, RF_B);
            deassertAllChipSelectPins();
        } else if (loCommand == LoCommand::RfBPowerMinus1) {
            LO->outputPower(-1, RF_B);
            deassertAllChipSelectPins();
        } else if (loCommand == LoCommand::RfBPowerPlus2) {
            LO->outputPower(2, RF_B);
            deassertAllChipSelectPins();
        } else if (loCommand == LoCommand::RfBPowerPlus5) {
            LO->outputPower(5, RF_B);
            deassertAllChipSelectPins();
        } else if (loCommand == LoCommand::FmnData) {
            setSerialPayloadMode(SerialPayloadMode::FMNData);
        }
        return;
    }
    if (commandCode == instructionWord(CmdDivaMode, AddrLo2) ||
        commandCode == instructionWord(CmdDivaMode, AddrLo3)) {
        return;
    }
    if (commandCode == instructionWord(CmdGeneral, AddrAdc1)) {
        selectChip(ChipTarget::ADC_1);
        return;
    }
    if (commandCode == instructionWord(CmdGeneral, AddrAdc2)) {
        selectChip(ChipTarget::ADC_2);
        return;
    }
    if (commandCode == instructionWord(CmdGeneral, AddrRam)) {
        selectChip(ChipTarget::RAM);
        return;
    }
    if (commandCode == instructionWord(CmdGeneral, AddrFlash)) {
        selectChip(ChipTarget::Flash);
        return;
    }
    if (commandCode == instructionWord(CmdFlashId, AddrFlash)) {
        Serial.print(F("Flash ID: 0x"));
        Serial.println(flashManufacturerId, HEX);
        Serial.print(F("Device ID: 0x"));
        Serial.println(flashDeviceId, HEX);
        return;
    }
    if (commandCode == instructionWord(CmdFlashRegisterReport, AddrFlash)) {
        Serial.print(F("Protection register report: 0x"));
        Serial.println(flashProtection, HEX);
        Serial.println();

        Serial.print(F("Configuration register report: 0x"));
        Serial.println(flashConfiguration, HEX);
        Serial.println();

        uint8_t status;
        flash.readStatus(RegAddrStatus, &status);
        Serial.print(F("Status register report: 0x"));
        Serial.println(status, HEX);
        Serial.println();
        return;
    }
    Serial.print(F("[WN2A] binary word 0x"));
    static const char hexDigits[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4) {
        const uint8_t nibble = static_cast<uint8_t>((word >> shift) & 0x0FU);
        Serial.print(hexDigits[nibble]);
    }
    Serial.println(F(" ignored."));
}

void handleFmnDataWord(uint32_t packedFMN)
{
    double* reportedFreq;
    const ChipTarget target = state.chipTarget;

    if (LO == nullptr) {
        return;
    }

    switch (target) {
        case ChipTarget::LO1:
            reportedFreq = &freqCalc.FreqLO1;
        case ChipTarget::LO2:
            reportedFreq = &freqCalc.FreqLO2;
        case ChipTarget::LO3:
            reportedFreq = &freqCalc.FreqLO3;
        default:
            break;
    }

    LO->setFrequency(packedFMN, LO->DIVA);
    *reportedFreq = LO->fmn2freq();               // Okay for testing only
}

void handleDirectRegisterDataWord(uint32_t dataWord)
{
    bool wroteTarget = true;
    switch (state.chipTarget) {
        case ChipTarget::LO1:
            halLo1.spiWriteRegister(dataWord);
            break;
        case ChipTarget::LO2:
            halLo2.spiWriteRegister(dataWord);
            break;
        case ChipTarget::LO3:
            halLo3.spiWriteRegister(dataWord);
            break;
        case ChipTarget::Attenuator:
            programAttenuatorRaw(static_cast<uint8_t>(dataWord & 0x7FU));
            break;
        case ChipTarget::ADC_1:
            spiWrite32(PIN_ADC_1, true, dataWord);
            break;
        case ChipTarget::ADC_2:
            spiWrite32(PIN_ADC_2, true, dataWord);
            break;
        case ChipTarget::RAM:
            spiWrite32(PIN_RAM, true, dataWord);
            break;
        case ChipTarget::Flash:
            spiWrite32(PIN_FLASH, true, dataWord);
            break;
        case ChipTarget::Off:
        case ChipTarget::None:
        default:
            wroteTarget = false;
            break;
    }
    if (wroteTarget) {
        deassertAllChipSelectPins();
    }
}

} // namespace

void processBinarySerialByte(uint8_t incomingByte)
{
    serialRxState.wordBytes[serialRxState.wordLength++] = incomingByte;
    if (serialRxState.wordLength < RECEIVED_WORD_BYTES) {
        return;
    }

    const uint32_t word =
        static_cast<uint32_t>(serialRxState.wordBytes[0]) |
        (static_cast<uint32_t>(serialRxState.wordBytes[1]) << 8) |
        (static_cast<uint32_t>(serialRxState.wordBytes[2]) << 16) |
        (static_cast<uint32_t>(serialRxState.wordBytes[3]) << 24);
    serialRxState.wordLength = 0U;
    processReceivedWord(word);
}

void processReceivedWord(uint32_t mode)
{
    // Check if the parsed 32-bit word has 0xFF (command flag) in its least-significant byte?
    const SerialPayloadMode modeType =
        ((mode & 0xFFU) == 0xFFU) ? SerialPayloadMode::Command : serialRxState.payloadMode;
    if (modeType == SerialPayloadMode::Command) {
        handleControlWord(mode);
    } else if (modeType == SerialPayloadMode::FMNData) {
        handleFmnDataWord(mode);
    } else {  // SerialPayloadMode::DirectRegisterData
        handleDirectRegisterDataWord(mode);
    }
}

// cppcheck-suppress unusedFunction
void processDirectRegisterData(uint32_t value)
{
    handleDirectRegisterDataWord(value);
}
