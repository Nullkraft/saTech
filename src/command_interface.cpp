#include "command_interface.h"
#include "console_state.h"

#include <SPI.h>
#include <arduino_hal.h>
#include <ctype.h>
#include <frequency_calculator.h>
#include <max2871.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace {

char inputBuffer[INPUT_BUFFER_SIZE];
size_t inputLength = 0;
ConsoleState& state = consoleState();
constexpr uint8_t RECEIVED_WORD_BYTES = 4U;

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
SerialTransportEncoding serialTransportEncoding = SerialTransportEncoding::Ascii;

bool equalsIgnoreCase(const char* lhs, const char* rhs)
{
    while (*lhs != '\0' && *rhs != '\0') {
        const char lc = static_cast<char>(tolower(*lhs));
        const char rc = static_cast<char>(tolower(*rhs));
        if (lc != rc) {
            return false;
        }
        ++lhs;
        ++rhs;
    }
    return (*lhs == '\0' && *rhs == '\0');
}

static inline double clampD(double v, double lo, double hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// I thought this was receiving 0..127 directly from the PC app?
uint8_t attenCodeFromDb(double db)
{
    const double clamped = clampD(db, ATTEN_MIN_DB, ATTEN_MAX_DB);
    const double steps = (clamped - ATTEN_MIN_DB) / ATTEN_STEP_DB;
    return static_cast<uint8_t>(lround(steps));
}


void programAttenuatorRaw(uint8_t code)
{
    // Ensure the attenuator CS is idle before starting the SPI transaction.
    // programAttenuatorRaw() manages its own complete CS cycle independently
    // of selectChip(); the two mechanisms do not interfere with each other.
    digitalWrite(PIN_ATTEN, LOW);
    SPI.beginTransaction(SPISettings(ATTEN_SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_ATTEN, HIGH);
    SPI.transfer(code);
    digitalWrite(PIN_ATTEN, LOW);
    SPI.endTransaction();
    const double mappedDb = ATTEN_MIN_DB + (static_cast<double>(code) * ATTEN_STEP_DB);
    if (mappedDb >= ATTEN_MIN_DB && mappedDb <= (ATTEN_MAX_DB + 0.25)) {
        state.attenuatorDb = mappedDb;
    }
}

// Generic 32-bit SPI write for targets without a dedicated HAL object.
// assertLow: true if the CS pin asserts LOW (ADC, RAM, Flash);
//            false if it asserts HIGH.
// cppcheck-suppress unusedFunction
void spiWrite32(uint8_t csPin, bool assertLow, uint32_t value)
{
    const uint8_t assertLevel   = assertLow ? LOW  : HIGH;
    const uint8_t deassertLevel = assertLow ? HIGH : LOW;
    SPI.beginTransaction(SPISettings(SPI_DEFAULT_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(csPin, assertLevel);
    SPI.transfer(static_cast<uint8_t>((value >> 24) & 0xFFU));
    SPI.transfer(static_cast<uint8_t>((value >> 16) & 0xFFU));
    SPI.transfer(static_cast<uint8_t>((value >>  8) & 0xFFU));
    SPI.transfer(static_cast<uint8_t>( value        & 0xFFU));
    digitalWrite(csPin, deassertLevel);
    SPI.endTransaction();
}

void handleControlWord(uint32_t word);
void handleFmnDataWord(uint32_t word);
void handleDirectRegisterDataWord(uint32_t word);
void collectBinaryByte(uint8_t incomingByte);
void collectAsciiByte(char incoming, uint8_t incomingByte);
bool parseAsciiControlWord(const char* token, uint32_t* word);
void applyReferenceSelection(ReferenceTarget target, bool verbose);

} // namespace

extern ArduinoHAL halLo1;
extern ArduinoHAL halLo2;
extern ArduinoHAL halLo3;
extern MAX2871 lo1;
extern MAX2871 lo2;
extern MAX2871 lo3;
extern FrequencyCalculator freqCalc;
extern double currentRfInputMhz;

void tuneTo(double mhz);
void printStatus();
void initializeLo(MAX2871& lo);

// Shared chip-select metadata: target, assigned pin, and asserted raw level.
const ChipSelectDefinition CHIP_DEFINITIONS[] = {
    {ChipTarget::Attenuator, PIN_ATTEN, HIGH},
    {ChipTarget::LO1,        PIN_LE_LO1, HIGH},
    {ChipTarget::LO2,        PIN_LE_LO2, HIGH},
    {ChipTarget::LO3,        PIN_LE_LO3, HIGH},
    {ChipTarget::RAM,        PIN_RAM,    LOW},
    {ChipTarget::Flash,      PIN_FLASH,  LOW},
    {ChipTarget::ADC1,       PIN_ADC1,   LOW},
    {ChipTarget::ADC2,       PIN_ADC2,   LOW},
};

const size_t CHIP_COUNT =
    sizeof(CHIP_DEFINITIONS) / sizeof(CHIP_DEFINITIONS[0]);

SaTech saTech;

bool SaTech::begin(const char* encoding)
{
    if (encoding == nullptr) {
        return false;
    }
    if (equalsIgnoreCase(encoding, "ascii")) {
        serialTransportEncoding = SerialTransportEncoding::Ascii;
        return true;
    } else if (equalsIgnoreCase(encoding, "binary")) {
        serialTransportEncoding = SerialTransportEncoding::Binary;
        return true;
    }
    return false;
}

bool SaTech::supportsEncoding(const char* encoding) const
{
    return encoding != nullptr &&
           (equalsIgnoreCase(encoding, "ascii") || equalsIgnoreCase(encoding, "binary"));
}

SerialTransportEncoding SaTech::transportEncoding() const
{
    return serialTransportEncoding;
}

const ChipSelectDefinition* chipSelectDefinitionForTarget(ChipTarget target)
{
    for (size_t i = 0; i < CHIP_COUNT; ++i) {
        if (CHIP_DEFINITIONS[i].chip == target) {
            return &CHIP_DEFINITIONS[i];
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// selectChip — public low-level primitive.
//
// Deasserts all CS/LE pins to their idle levels, then asserts the requested
// target's pin. ChipTarget::None deasserts everything and leaves it that way.
// Resets the SPI arming state on every call so a chip switch cannot carry over
// a previously armed write.
//
// This function owns state.chipTarget and is the only site that writes it.
// It is intentionally side-effect-free beyond pin state and state.chipTarget.
// ---------------------------------------------------------------------------
void selectChip(ChipTarget target)
{
    for (size_t i = 0; i < CHIP_COUNT; ++i) {
        const ChipSelectDefinition& def = CHIP_DEFINITIONS[i];
        digitalWrite(def.pin, (def.assertedLevel == HIGH) ? LOW : HIGH);
    }

    const ChipSelectDefinition* selected = chipSelectDefinitionForTarget(target);
    if (selected != nullptr) {
        digitalWrite(selected->pin, selected->assertedLevel);
    }

    ConsoleState& s = consoleState();
    s.chipTarget            = target;
    s.manualSpiArmed        = false;
    s.pendingSpiConfirmation = false;

    if (target == ChipTarget::None) {
        Serial.println(F("All chip selects deasserted."));
    } else {
        Serial.print(F("Chip select set to "));
        Serial.println(chipTargetName(target));
    }
}

// ---------------------------------------------------------------------------
// selectRef — public low-level primitive.
//
// Deasserts both REF_EN pins, then asserts the requested reference clock.
// ChipTarget::None disables both clocks (with a warning — LOs will lose lock).
// Any target value other than Ref1, Ref2, or None is rejected with an error.
//
// This function owns state.ref1Enabled and state.ref2Enabled and is the only
// site that writes those flags.
// It is intentionally side-effect-free beyond pin state and those two flags.
// ---------------------------------------------------------------------------
void selectRef(ReferenceTarget target)
{
    applyReferenceSelection(target, true);
}

int readOutputPinLevel(uint8_t pin)
{
    return digitalRead(pin);
}

// The serial stream is byte-oriented, and higher-level things like ASCII lines
// or 4-byte binary words are assembled from those individual bytes.
//
void pollSerial()
{
    while (Serial.available() > 0) {
        const char incomingChar = static_cast<char>(Serial.read());
        const uint8_t incomingByte = static_cast<uint8_t>(incomingChar);
        if (serialTransportEncoding == SerialTransportEncoding::Binary) {
            collectBinaryByte(incomingByte);
            continue;
        }
        collectAsciiByte(incomingChar, incomingByte);
    }
}

// cppcheck-suppress unusedFunction
void programAttenuatorDb(double db)
{
    const uint8_t code = attenCodeFromDb(db);
    programAttenuatorRaw(code);
    state.attenuatorDb = ATTEN_MIN_DB + (static_cast<double>(code) * ATTEN_STEP_DB);
}

// cppcheck-suppress unusedFunction
double getCurrentAttenuatorDb()
{
    return state.attenuatorDb;
}

// cppcheck-suppress unusedFunction
ChipTarget getCurrentChipTarget()
{
    return state.chipTarget;
}

const __FlashStringHelper* chipTargetName(ChipTarget target)
{
    switch (target) {
        case ChipTarget::LO1:        return F("LO1");
        case ChipTarget::LO2:        return F("LO2");
        case ChipTarget::LO3:        return F("LO3");
        case ChipTarget::Attenuator: return F("Attenuator");
        case ChipTarget::ADC1:       return F("ADC1");
        case ChipTarget::ADC2:       return F("ADC2");
        case ChipTarget::RAM:        return F("RAM");
        case ChipTarget::Flash:      return F("FLASH");
        case ChipTarget::None:
        default:                     return F("None");
    }
}

namespace {

void collectBinaryByte(uint8_t incomingByte)
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
    serialRxState.wordLength = 0;
    processReceivedWord(word);
}

void collectAsciiByte(char incoming, uint8_t incomingByte)
{
    if (incoming != '\r' && incoming != '\n' && isprint(incomingByte) == 0) {
        return;
    }
    if (incoming == '\r') {
        return;
    }
    if (incoming == '\n') {
        inputBuffer[inputLength] = '\0';
        uint32_t word = 0U;
        if (parseAsciiControlWord(inputBuffer, &word)) {
            processReceivedWord(word);
        }
        inputLength = 0;
        return;
    }
    if (inputLength < (INPUT_BUFFER_SIZE - 1U)) {
        inputBuffer[inputLength++] = incoming;
        return;
    }
    inputLength = 0;
    Serial.println(F("Input too long, line cleared."));
}

void applyReferenceSelection(ReferenceTarget target, bool verbose)
{
    ConsoleState& s = consoleState();

    // Deassert both reference clocks unconditionally.
    digitalWrite(PIN_REF_EN1, LOW);
    digitalWrite(PIN_REF_EN2, LOW);
    s.ref1Enabled = false;
    s.ref2Enabled = false;

    switch (target) {
    case ReferenceTarget::Ref1:
        digitalWrite(PIN_REF_EN1, HIGH);
        s.ref1Enabled = true;
        if (verbose) {
            Serial.println(F("Reference clock set to REF1."));
        }
        break;
    case ReferenceTarget::Ref2:
        digitalWrite(PIN_REF_EN2, HIGH);
        s.ref2Enabled = true;
        if (verbose) {
            Serial.println(F("Reference clock set to REF2."));
        }
        break;
    case ReferenceTarget::None:
        // ReferenceTarget::None — both clocks remain deasserted.
        if (verbose) {
            Serial.println(F("Warning: all reference clocks disabled — LOs will lose lock."));
        }
        break;
    default:
        break;
    }
}

} // namespace

void selectSerialChipTarget(ChipTarget target)
{
    serialRxState.selectedBinaryTarget = target;
    selectChip(target);
}

namespace {

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
            serialTransportEncoding = SerialTransportEncoding::Ascii;
            return;
        }
        if (word == 0x000206FFUL) {
            serialTransportEncoding = SerialTransportEncoding::Binary;
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
        printStatus();
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
        applyReferenceSelection(ReferenceTarget::None, false);
        return;
    }
    if (selector == 0x0CFFU) {
        applyReferenceSelection(ReferenceTarget::Ref1, false);
        return;
    }
    if (selector == 0x14FFU) {
        applyReferenceSelection(ReferenceTarget::Ref2, false);
        return;
    }
    ChipTarget loTarget = ChipTarget::None;
    if (loTargetForControlSelector(selector, &loTarget)) {
        selectSerialChipTarget(loTarget);
        return;
    }
    if (selector == 0x08FFU) {
        programAttenuatorRaw(static_cast<uint8_t>((word >> 16) & 0x7FU));
        return;
    }
    if (loTargetForListedSelector(selector, 0x08U, &loTarget)) {
        applyLoOutputSelect(loTarget, RFNONE);
        return;
    }
    if (loTargetForListedSelector(selector, 0x10U, &loTarget)) {
        applyLoOutputPower(loTarget, -4);
        return;
    }
    if (loTargetForListedSelector(selector, 0x18U, &loTarget)) {
        applyLoOutputPower(loTarget, -1);
        return;
    }
    if (loTargetForListedSelector(selector, 0x20U, &loTarget)) {
        applyLoOutputPower(loTarget, 2);
        return;
    }
    if (loTargetForListedSelector(selector, 0x28U, &loTarget)) {
        applyLoOutputPower(loTarget, 5);
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
        selectSerialChipTarget(ChipTarget::ADC1);
        return;
    }
    if (selector == 0x0DFFU) {
        selectSerialChipTarget(ChipTarget::ADC2);
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
    Serial.print(F(" ignored."));
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
}

void handleDirectRegisterDataWord(uint32_t chipSelPin)
{
    switch (serialRxState.selectedBinaryTarget) {
        case ChipTarget::LO1:
            halLo1.spiWriteRegister(chipSelPin);
            break;
        case ChipTarget::LO2:
            halLo2.spiWriteRegister(chipSelPin);
            break;
        case ChipTarget::LO3:
            halLo3.spiWriteRegister(chipSelPin);
            break;
        case ChipTarget::Attenuator:
            programAttenuatorRaw(static_cast<uint8_t>(chipSelPin & 0x7FU));
            break;
        case ChipTarget::ADC1:
            spiWrite32(PIN_ADC1, true, chipSelPin);
            break;
        case ChipTarget::ADC2:
            spiWrite32(PIN_ADC2, true, chipSelPin);
            break;
        case ChipTarget::RAM:
            spiWrite32(PIN_RAM, true, chipSelPin);
            break;
        case ChipTarget::Flash:
            spiWrite32(PIN_FLASH, true, chipSelPin);
            break;
        case ChipTarget::None:
        default:
            break;
    }
}

} // namespace

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
    const SerialPayloadMode previousMode = serialRxState.payloadMode;
    serialRxState.selectedBinaryTarget = state.chipTarget;
    processReceivedWord(0x16FFU);
    if ((value & 0xFFU) == 0xFFU) {
        handleDirectRegisterDataWord(value);
    } else {
        processReceivedWord(value);
    }
    setSerialPayloadMode(previousMode);
}

namespace {

// Convert string, e.g. "17ff", to hex 0x17FF
bool parseAsciiControlWord(const char* token, uint32_t* word)
{
    if (token == nullptr || word == nullptr) {
        return false;
    }
    char* endPointer = nullptr;
    *word = static_cast<uint32_t>(strtoul(token, &endPointer, 16)); // Cast to 32 bits
    return endPointer != token && *endPointer == '\0';
}
} // namespace
