#include "command_interface.h"

#include "binary_command_executor.h"
#include "board_devices.h"
#include "board_control.h"
#include "console_state.h"
#include "technician_console.h"

#include <SPI.h>

namespace {

constexpr uint8_t RECEIVED_WORD_BYTES = 4U;
ConsoleState& state = consoleState();

enum class SerialPayloadMode {
    Command,
    FMNData,
    DirectRegisterData,
};

struct SerialReceiveState {
    SerialPayloadMode payloadMode;
    ChipTarget currentlySelectedChip;
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
    ChipTarget::None,
    {0U, 0U, 0U, 0U},
    0U,
};

bool loTargetForControlCode(uint16_t commandCode, ChipTarget* target)
{
    if (commandCode == 0x01FFU) {
        *target = ChipTarget::LO1;
        return true;
    }
    if (commandCode == 0x02FFU) {
        *target = ChipTarget::LO2;
        return true;
    }
    if (commandCode == 0x03FFU) {
        *target = ChipTarget::LO3;
        return true;
    }
    return false;   // Handles command codes that are not LO control codes
}

ChipTarget getChipTarget(uint8_t chipAddress)
{
    if (chipAddress == 1U) {
        return ChipTarget::LO1;
    }
    if (chipAddress == 2U) {
        return ChipTarget::LO2;
    }
    if (chipAddress == 3U) {
        return ChipTarget::LO3;
    }
    return ChipTarget::None;
}

LoCommand getLoCommand(uint8_t commandBits)
{
    if (commandBits == 0x01U) return LoCommand::DisableOutput;
    if (commandBits == 0x02U) return LoCommand::RfBPowerMinus4;
    if (commandBits == 0x03U) return LoCommand::RfBPowerMinus1;
    if (commandBits == 0x04U) return LoCommand::RfBPowerPlus2;
    if (commandBits == 0x05U) return LoCommand::RfBPowerPlus5;
    if (commandBits == 0x06U) return LoCommand::FmnData;
    if (commandBits == 0x07U || commandBits == 0x08U) return LoCommand::Ignored;
    return LoCommand::None;
}

bool decodeLoCommand(uint16_t commandCode, LoCommand* loCommand)
{
    if ((commandCode & 0x00FFU) != 0x00FFU) {
        return false;
    }
    const uint8_t commandByte = static_cast<uint8_t>((commandCode >> 8) & 0xFFU);
    const uint8_t chipAddress = commandByte & 0x07U;
    const uint8_t commandBits = commandByte >> 3;

    const ChipTarget target = getChipTarget(chipAddress);
    if (target == ChipTarget::None) {
        return false;
    }

    const LoCommand command = getLoCommand(commandBits);
    if (command == LoCommand::None) {
        return false;
    }

    serialRxState.currentlySelectedChip = target;
    *loCommand = command;
    return true;
}

MAX2871* targetLo(ChipTarget target)
{
    switch (target) {
        case ChipTarget::LO1:
            return &lo1;
        case ChipTarget::LO2:
            return &lo2;
        case ChipTarget::LO3:
            return &lo3;
        default:
            break;
    }
    return nullptr;
}

double* reportedFrequencyForTargetLo(ChipTarget target)
{
    switch (target) {
        case ChipTarget::LO1:
            return &freqCalc.FreqLO1;
        case ChipTarget::LO2:
            return &freqCalc.FreqLO2;
        case ChipTarget::LO3:
            return &freqCalc.FreqLO3;
        default:
            break;
    }
    return nullptr;
}

void setSerialPayloadMode(SerialPayloadMode mode)
{
    serialRxState.payloadMode = mode;
    serialRxState.wordLength = 0U;
}

void deassertProgrammingPins()
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
        if (word == 0x000106FFUL) {
            setSerialEncoding(SerialEncoding::Ascii);
            return;
        }
        if (word == 0x000206FFUL) {
            setSerialEncoding(SerialEncoding::Binary);
            return;
        }
    }
    if (commandCode == 0x0FFFU) {
        digitalWrite(LED_BUILTIN, HIGH);
        Serial.print(F("LED on"));
        return;
    }
    if (commandCode == 0x07FFU) {
        digitalWrite(LED_BUILTIN, LOW);
        Serial.print(F("LED off"));
        return;
    }
    if (commandCode == 0x17FFU) {
        Serial.print(F("saTech WN2A ready"));
        return;
    }
    // Unimplemented - Begin/End Macro, Begin/End Sweep, and Squelch Level
    if (commandCode == 0x1FFFU || commandCode == 0x27FFU ||
        commandCode == 0x37FFU || commandCode == 0x3FFFU ||
        commandCode == 0x47FFU) {
        return;
    }
    if (commandCode == 0x2FFFU) {
        initializeLo(lo1);
        initializeLo(lo2);
        initializeLo(lo3);
        tuneTo(currentRfInputMhz);
        printFulltestPlanReport();
        return;
    }
    if (commandCode == 0x06FFU) {
        setSerialPayloadMode(SerialPayloadMode::Command);
        return;
    }
    if (commandCode == 0x0EFFU) {
        setSerialPayloadMode(SerialPayloadMode::FMNData);
        return;
    }
    if (commandCode == 0x16FFU) {
        setSerialPayloadMode(SerialPayloadMode::DirectRegisterData);
        return;
    }
    if (commandCode == 0x04FFU) {
        selectRef(ReferenceTarget::Off);
        return;
    }
    if (commandCode == 0x0CFFU) {
        selectRef(ReferenceTarget::Ref1);
        return;
    }
    if (commandCode == 0x14FFU) {
        selectRef(ReferenceTarget::Ref2);
        return;
    }
    ChipTarget loTarget;
    if (loTargetForControlCode(commandCode, &loTarget)) {
        selectSerialChipTarget(loTarget);
        return;
    }
    if (commandCode == 0x08FFU) {
        programAttenuatorRaw(static_cast<uint8_t>((word >> 16) & 0x7FU));
        deassertProgrammingPins();
        return;
    }
    LoCommand loCommand;
    if (decodeLoCommand(commandCode, &loCommand)) {
        MAX2871* lo = targetLo(serialRxState.currentlySelectedChip);
        if (loCommand == LoCommand::DisableOutput) {
            lo->outputSelect(RFNONE);
            deassertProgrammingPins();
        } else if (loCommand == LoCommand::RfBPowerMinus4) {
            lo->outputPower(-4, RF_B);
            deassertProgrammingPins();
        } else if (loCommand == LoCommand::RfBPowerMinus1) {
            lo->outputPower(-1, RF_B);
            deassertProgrammingPins();
        } else if (loCommand == LoCommand::RfBPowerPlus2) {
            lo->outputPower(2, RF_B);
            deassertProgrammingPins();
        } else if (loCommand == LoCommand::RfBPowerPlus5) {
            lo->outputPower(5, RF_B);
            deassertProgrammingPins();
        } else if (loCommand == LoCommand::FmnData) {
            selectSerialChipTarget(serialRxState.currentlySelectedChip);
            setSerialPayloadMode(SerialPayloadMode::FMNData);
        }
        return;
    }
    if (commandCode == 0x4AFFU || commandCode == 0x4BFFU) {
        return;
    }
    if (commandCode == 0x05FFU) {
        selectSerialChipTarget(ChipTarget::ADC_1);
        return;
    }
    if (commandCode == 0x0DFFU) {
        selectSerialChipTarget(ChipTarget::ADC_2);
        return;
    }
    if (commandCode == 0x15FFU) {
        selectSerialChipTarget(ChipTarget::RAM);
        return;
    }
    if (commandCode == 0x48FFU) {
        Serial.print(F("Flash ID: 0x"));
        Serial.println(flash.getManufID(), HEX);
        Serial.print(F("Device ID: 0x"));
        Serial.println(flash.getDeviceID(), HEX);
        return;
    }
    if (commandCode == 0x49FFU) {
        Serial.print(F("Protection register report: 0x"));
        Serial.println(flash.getProtReg(), HEX);
        Serial.println();

        Serial.print(F("Configuration register report: 0x"));
        Serial.println(flash.getConfReg(), HEX);
        Serial.println();

        Serial.print(F("Status register report: 0x"));
        Serial.println(flash.getStatReg(), HEX);
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
    const ChipTarget target = serialRxState.currentlySelectedChip;
    MAX2871* lo = targetLo(target);
    if (lo == nullptr) {
        return;
    }
    double* reportedFreq = reportedFrequencyForTargetLo(target);

    lo->setFrequency(packedFMN, lo->DIVA);
    *reportedFreq = lo->fmn2freq();               // Okay for testing only

    if (target == ChipTarget::LO1) {
        state.lo1Manual = true;
    } else if (target == ChipTarget::LO2) {
        state.lo2Manual = true;
    } else {  // ChipTarget::LO3
        state.lo3Manual = true;
    }

    deassertProgrammingPins();
}

void handleDirectRegisterDataWord(uint32_t dataWord)
{
    bool wroteTarget = true;
    switch (serialRxState.currentlySelectedChip) {
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
        deassertProgrammingPins();
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

void selectSerialChipTarget(ChipTarget target)
{
    serialRxState.currentlySelectedChip = target;
    selectChip(target);
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
    serialRxState.currentlySelectedChip = state.chipTarget;
    handleDirectRegisterDataWord(value);
}
