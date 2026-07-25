#include "command_interface.h"

#include "binary_command_executor.h"
#include "board_control.h"
#include "console_state.h"
#include "technician_console.h"

#include <SPI.h>
#include <arduino_hal.h>
#include <frequency_calculator.h>
#include <max2871.h>

extern ArduinoHAL halLo1;
extern ArduinoHAL halLo2;
extern ArduinoHAL halLo3;
extern MAX2871 lo1;
extern MAX2871 lo2;
extern MAX2871 lo3;
extern FrequencyCalculator freqCalc;
extern double currentRfInputMhz;

void tuneTo(double mhz);
void initializeLo(MAX2871& lo);

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
    ChipTarget selectedBinaryTarget;
    uint8_t wordBytes[RECEIVED_WORD_BYTES];
    uint8_t wordLength;
};

SerialReceiveState serialRxState = {
    SerialPayloadMode::Command,
    ChipTarget::None,
    {0U, 0U, 0U, 0U},
    0U,
};

bool loTargetForControlSelector(uint16_t selector, ChipTarget* target)
{
    if (target == nullptr) {
        return false;
    }
    if (selector == 0x01FFU) {
        *target = ChipTarget::LO1;
        return true;
    }
    if (selector == 0x02FFU) {
        *target = ChipTarget::LO2;
        return true;
    }
    if (selector == 0x03FFU) {
        *target = ChipTarget::LO3;
        return true;
    }
    return false;
}

bool loTargetForListedSelector(uint16_t selector, uint8_t baseCommand, ChipTarget* target)
{
    if (target == nullptr || (selector & 0x00FFU) != 0x00FFU) {
        return false;
    }
    const uint8_t command = static_cast<uint8_t>((selector >> 8) & 0xFFU);
    if (command < (baseCommand + 1U) || command > (baseCommand + 3U)) {
        return false;
    }
    switch (command - baseCommand) {
        case 1U: *target = ChipTarget::LO1; return true;
        case 2U: *target = ChipTarget::LO2; return true;
        case 3U: *target = ChipTarget::LO3; return true;
        default: break;
    }
    return false;
}

bool loStateForTarget(ChipTarget target, MAX2871** targetLo, double** reportedFreq)
{
    if (targetLo == nullptr || reportedFreq == nullptr) {
        return false;
    }
    switch (target) {
        case ChipTarget::LO1:
            *targetLo = &lo1;
            *reportedFreq = &freqCalc.FreqLO1;
            return true;
        case ChipTarget::LO2:
            *targetLo = &lo2;
            *reportedFreq = &freqCalc.FreqLO2;
            return true;
        case ChipTarget::LO3:
            *targetLo = &lo3;
            *reportedFreq = &freqCalc.FreqLO3;
            return true;
        default:
            break;
    }
    return false;
}

void markLoManual(ChipTarget target)
{
    switch (target) {
        case ChipTarget::LO1: state.lo1Manual = true; break;
        case ChipTarget::LO2: state.lo2Manual = true; break;
        case ChipTarget::LO3: state.lo3Manual = true; break;
        default: break;
    }
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
    state.chipTarget = ChipTarget::None;
    state.manualSpiArmed = false;
    state.pendingSpiConfirmation = false;
}

void applyLoOutputSelect(ChipTarget target, RFOutPort port)
{
    MAX2871* targetLo = nullptr;
    double* reportedFreq = nullptr;
    if (loStateForTarget(target, &targetLo, &reportedFreq)) {
        targetLo->outputSelect(port);
    }
}

void applyLoOutputPower(ChipTarget target, int dBm)
{
    MAX2871* targetLo = nullptr;
    double* reportedFreq = nullptr;
    if (loStateForTarget(target, &targetLo, &reportedFreq)) {
        targetLo->outputPower(dBm, RF_B);
    }
}

void handleControlWord(uint32_t word)
{
    const uint16_t selector = static_cast<uint16_t>(word & 0xFFFFU);
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
    if (selector == 0x0FFFU) {
        digitalWrite(LED_BUILTIN, HIGH);
        Serial.print(F("LED on"));
        return;
    }
    if (selector == 0x07FFU) {
        digitalWrite(LED_BUILTIN, LOW);
        Serial.print(F("LED off"));
        return;
    }
    if (selector == 0x17FFU) {
        Serial.print(F("saTech WN2A ready"));
        return;
    }
    // Unimplemented - Begin/End Macro, Begin/End Sweep, and Squelch Level
    if (selector == 0x1FFFU || selector == 0x27FFU ||
        selector == 0x37FFU || selector == 0x3FFFU ||
        selector == 0x47FFU) {
        return;
    }
    if (selector == 0x2FFFU) {
        initializeLo(lo1);
        initializeLo(lo2);
        initializeLo(lo3);
        tuneTo(currentRfInputMhz);
        printFulltestPlanReport();
        return;
    }
    if (selector == 0x06FFU) {
        setSerialPayloadMode(SerialPayloadMode::Command);
        return;
    }
    if (selector == 0x0EFFU) {
        setSerialPayloadMode(SerialPayloadMode::FMNData);
        return;
    }
    if (selector == 0x16FFU) {
        setSerialPayloadMode(SerialPayloadMode::DirectRegisterData);
        return;
    }
    if (selector == 0x04FFU) {
        selectRef(ReferenceTarget::None);
        return;
    }
    if (selector == 0x0CFFU) {
        selectRef(ReferenceTarget::Ref1);
        return;
    }
    if (selector == 0x14FFU) {
        selectRef(ReferenceTarget::Ref2);
        return;
    }
    ChipTarget loTarget = ChipTarget::None;
    if (loTargetForControlSelector(selector, &loTarget)) {
        selectSerialChipTarget(loTarget);
        return;
    }
    if (selector == 0x08FFU) {
        programAttenuatorRaw(static_cast<uint8_t>((word >> 16) & 0x7FU));
        deassertProgrammingPins();
        return;
    }
    if (loTargetForListedSelector(selector, 0x08U, &loTarget)) {
        applyLoOutputSelect(loTarget, RFNONE);
        deassertProgrammingPins();
        return;
    }
    if (loTargetForListedSelector(selector, 0x10U, &loTarget)) {
        applyLoOutputPower(loTarget, -4);
        deassertProgrammingPins();
        return;
    }
    if (loTargetForListedSelector(selector, 0x18U, &loTarget)) {
        applyLoOutputPower(loTarget, -1);
        deassertProgrammingPins();
        return;
    }
    if (loTargetForListedSelector(selector, 0x20U, &loTarget)) {
        applyLoOutputPower(loTarget, 2);
        deassertProgrammingPins();
        return;
    }
    if (loTargetForListedSelector(selector, 0x28U, &loTarget)) {
        applyLoOutputPower(loTarget, 5);
        deassertProgrammingPins();
        return;
    }
    if (loTargetForListedSelector(selector, 0x30U, &loTarget)) {
        selectSerialChipTarget(loTarget);
        setSerialPayloadMode(SerialPayloadMode::FMNData);
        return;
    }
    if (loTargetForListedSelector(selector, 0x38U, &loTarget) ||
        loTargetForListedSelector(selector, 0x40U, &loTarget) ||
        selector == 0x4AFFU || selector == 0x4BFFU) {
        return;
    }
    if (selector == 0x05FFU) {
        selectSerialChipTarget(ChipTarget::ADC_1);
        return;
    }
    if (selector == 0x0DFFU) {
        selectSerialChipTarget(ChipTarget::ADC_2);
        return;
    }
    if (selector == 0x15FFU) {
        selectSerialChipTarget(ChipTarget::RAM);
        return;
    }
    if (selector == 0x1DFFU) {
        selectSerialChipTarget(ChipTarget::Flash);
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
    MAX2871* targetLo = nullptr;
    double* reportedFreq = nullptr;
    const ChipTarget target = serialRxState.selectedBinaryTarget;
    if (!loStateForTarget(target, &targetLo, &reportedFreq)) {
        return;
    }

    targetLo->setFrequency(packedFMN, targetLo->DIVA);
    *reportedFreq = targetLo->fmn2freq();               // Okay for testing only
    markLoManual(target);
    deassertProgrammingPins();
}

void handleDirectRegisterDataWord(uint32_t dataWord)
{
    bool wroteTarget = true;
    switch (serialRxState.selectedBinaryTarget) {
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
    serialRxState.selectedBinaryTarget = target;
    selectChip(target);
}

void processReceivedWord(uint32_t mode)
{
    // Check if the parsed 32-bit word has 0xFF (command flag) in its least-significant byte?
    const SerialPayloadMode modeType =
        ((mode & 0xFFU) == 0xFFU) ? SerialPayloadMode::Command : serialRxState.payloadMode;
    switch (modeType) {
        case SerialPayloadMode::Command:
            handleControlWord(mode);
            break;
        case SerialPayloadMode::FMNData:
            handleFmnDataWord(mode);
            break;
        case SerialPayloadMode::DirectRegisterData:
            handleDirectRegisterDataWord(mode);
            break;
        default:
            break;
    }
}

// cppcheck-suppress unusedFunction
void processDirectRegisterData(uint32_t value)
{
    serialRxState.selectedBinaryTarget = state.chipTarget;
    handleDirectRegisterDataWord(value);
}
