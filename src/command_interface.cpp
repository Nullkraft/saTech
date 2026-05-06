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

enum class SerialRxMode {
    AsciiLineData,     // Collect technician ASCII commands until a line terminator.
    BinaryControlWord, // Collect one four-byte WN2A command & control frame.
    FMNData,           // Collect packed MAX2871 FMN words for the selected LO.
    Direct2Register,   // Collect raw register words for the selected SPI target.
};

struct SerialReceiveState {
    SerialRxMode mode;
    ChipTarget selectedBinaryTarget;
    SerialRxMode pendingWordType;
    uint8_t wordBytes[4];
    uint8_t wordLength;
};

SerialReceiveState serialRxState = {
    SerialRxMode::AsciiLineData,
    ChipTarget::None,
    SerialRxMode::BinaryControlWord,
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

enum class CommandKind {
    Unknown,
    Help,
    Status,
    Relock,
    Info,
    Atten,
    Ifmode,
    Lofreq,
    Chip,
    Set,
    Spi,
};

void trimWhitespace(char* text)
{
    if (text == nullptr) {
        return;
    }
    char* start = text;
    while (*start != '\0' && isspace(*start) != 0) {
        ++start;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1U);
    }
    size_t len = strlen(text);
    while (len > 0U && isspace(text[len - 1U]) != 0) {
        text[len - 1U] = '\0';
        --len;
    }
}

uint8_t attenCodeFromDb(double db)
{
    double clamped = db;
    if (clamped < ATTEN_MIN_DB) {
        clamped = ATTEN_MIN_DB;
    } else if (clamped > ATTEN_MAX_DB) {
        clamped = ATTEN_MAX_DB;
    }
    const double steps = (clamped - ATTEN_MIN_DB) / ATTEN_STEP_DB;
    const int32_t code = static_cast<int32_t>(lround(steps));
    if (code < 0) {
        return 0U;
    }
    if (code > 0x7FU) {
        return 0x7FU;
    }
    return static_cast<uint8_t>(code);
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

void logManualWrite(uint32_t value)
{
    Serial.print(F("[SPI] target="));
    Serial.print(chipTargetName(state.chipTarget));
    Serial.print(F(" value=0x"));
    static const char hexDigits[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4) {
        const uint8_t nibble = static_cast<uint8_t>((value >> shift) & 0x0FU);
        Serial.print(hexDigits[nibble]);
    }
    Serial.println();
}

void handleAttenuatorCommand(const char* valueToken);
void handleIfmodeCommand(const char* modeToken);
void handleLofreqCommand(const char* valueToken);
void handleChipCommand(const char* targetToken);
void handleSetCommand(const char* targetToken);
void handleSpiCommand(const char* valueToken);
void handleCommand(const char* line);
void handleControlWord(uint32_t word);
void handleFmnDataWord(uint32_t word);
bool isBinaryWordCollectionInProgress();
bool isBinaryControlWordStartByte(uint8_t incomingByte, bool atAsciiFrameBoundary);
bool isModeDataWordExpected();
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

void SaTech::begin(const char* encoding)
{
    if (equalsIgnoreCase(encoding, "ascii")) {
        serialTransportEncoding = SerialTransportEncoding::Ascii;
    } else if (equalsIgnoreCase(encoding, "binary")) {
        serialTransportEncoding = SerialTransportEncoding::Binary;
    }
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

const __FlashStringHelper* injectionLabel(LOInjectionMode mode)
{
    return (mode == LOInjectionMode::High) ? F("High") : F("Low");
}

void printInjectionSummary()
{
    const ConsoleState& s = consoleState();
    Serial.print(F("Injection: LO1="));
    Serial.print(s.lo1Manual ? F("Manual") : injectionLabel(freqCalc.LO1InjectionMode));
    Serial.print(F(" LO2="));
    Serial.print(s.lo2Manual ? F("Manual") : injectionLabel(freqCalc.LO2InjectionMode));
    Serial.print(F(" LO3="));
    Serial.println(s.lo3Manual ? F("Manual") : injectionLabel(freqCalc.LO3InjectionMode));
}

void printSelectedLoSnapshot(ChipTarget target)
{
    const MAX2871* targetLo = nullptr;
    const __FlashStringHelper* label = nullptr;
    double freq = 0.0;
    switch (target) {
        case ChipTarget::LO1:
            targetLo = &lo1;
            label = F("LO1");
            freq = freqCalc.FreqLO1;
            break;
        case ChipTarget::LO2:
            targetLo = &lo2;
            label = F("LO2");
            freq = freqCalc.FreqLO2;
            break;
        case ChipTarget::LO3:
            targetLo = &lo3;
            label = F("LO3");
            freq = freqCalc.FreqLO3;
            break;
        default:
            return;
    }
    Serial.println();
    Serial.print(label);
    Serial.print(F("  : "));
    Serial.print(freq, 3);
    Serial.println(F(" MHz"));
    Serial.print(label);
    Serial.print(F(" M="));
    Serial.print(targetLo->M);
    Serial.print(F(" F="));
    Serial.print(targetLo->Frac);
    Serial.print(F(" N="));
    Serial.print(targetLo->N);
    Serial.print(F(" DIVA="));
    Serial.println(1 << targetLo->DIVA);
}

void tuneTo(double mhz);
void printStatus();
void initializeLo(MAX2871& lo);
void recomputePlan();

void printBanner()
{
    Serial.println();
    Serial.println(F("=== SpecAnn Technician Console ==="));
    Serial.println(F("Commands:"));
    Serial.println(F("  RFin <MHz>            Tune all 3 LO's (23.5 to 6000 MHz)"));
    Serial.println(F("  help                  Show this list"));
    Serial.println(F("  status                Report LO/IF plan, attenuator state, chip target"));
    Serial.println(F("  relock                Reinitialize MAX2871 devices"));
    Serial.println(F("  info                  Show board pin assignments"));
    Serial.println(F("  atten <dB>            Program PE43711 attenuator (0.0 to 31.75 dB in 0.25 steps)"));
    Serial.println(F("  ifmode <high|low>     Set injection for the selected LO (use chip first)"));
    Serial.println(F("  lofreq <MHz>          Program the selected LO directly"));
    Serial.println(F("  chip <target|off>     Assert one CS/LE pin; off deasserts all"));
    Serial.println(F("    targets: lo1 lo2 lo3 atten adc1 adc2 ram flash"));
    Serial.println(F("  set <ref1|ref2|off>   Enable one reference clock; off disables both"));
    Serial.println(F("  spi <hex32>           Send raw 32-bit word to selected device"));
    Serial.println();
}

// The serial stream is byte-oriented, and higher-level things like ASCII lines
// or 4-byte binary words are assembled from those individual bytes.
//
void pollSerial()
{
    while (Serial.available() > 0) {
        const char incomingChar = static_cast<char>(Serial.read());
        const uint8_t incomingByte = static_cast<uint8_t>(incomingChar);
        if (isBinaryControlWordStartByte(incomingByte, inputLength == 0U) ||
            isBinaryWordCollectionInProgress() ||
            isModeDataWordExpected()) {
            collectBinaryByte(incomingByte);
            continue;
        }
        collectAsciiByte(incomingChar, incomingByte);
    }
}

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

bool isBinaryWordCollectionInProgress()
{
    return (serialRxState.wordLength > 0U);
}

bool isBinaryControlWordStartByte(uint8_t incomingByte, bool atAsciiFrameBoundary)
{
    return (incomingByte == 0xFFU) &&
           (serialRxState.mode != SerialRxMode::AsciiLineData || atAsciiFrameBoundary);
}

bool isModeDataWordExpected()
{
    return (serialRxState.mode == SerialRxMode::FMNData);
}

void collectBinaryByte(uint8_t incomingByte)
{
    if (serialRxState.wordLength == 0U) {
        serialRxState.pendingWordType =
            (incomingByte == 0xFFU) ? SerialRxMode::BinaryControlWord : serialRxState.mode;
    }
    serialRxState.wordBytes[serialRxState.wordLength++] = incomingByte;
    if (serialRxState.wordLength < 4U) {
        return;
    }

    const uint32_t word =
        static_cast<uint32_t>(serialRxState.wordBytes[0]) |
        (static_cast<uint32_t>(serialRxState.wordBytes[1]) << 8) |
        (static_cast<uint32_t>(serialRxState.wordBytes[2]) << 16) |
        (static_cast<uint32_t>(serialRxState.wordBytes[3]) << 24);
    serialRxState.wordLength = 0;
    if (serialRxState.pendingWordType == SerialRxMode::BinaryControlWord) {
        handleControlWord(word);
    } else if (serialRxState.pendingWordType == SerialRxMode::FMNData) {
        handleFmnDataWord(word);
    }
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
        handleCommand(inputBuffer);
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
    if (target != ReferenceTarget::Ref1 &&
        target != ReferenceTarget::Ref2 &&
        target != ReferenceTarget::None) {
        if (verbose) {
            Serial.println(F("set requires ref1, ref2, or off."));
        }
        return;
    }

    ConsoleState& s = consoleState();

    // Deassert both reference clocks unconditionally.
    digitalWrite(PIN_REF_EN1, LOW);
    digitalWrite(PIN_REF_EN2, LOW);

    s.ref1Enabled = false;
    s.ref2Enabled = false;

    if (target == ReferenceTarget::Ref1) {
        digitalWrite(PIN_REF_EN1, HIGH);
        s.ref1Enabled = true;
        if (verbose) {
            Serial.println(F("Reference clock set to REF1."));
        }
    } else if (target == ReferenceTarget::Ref2) {
        digitalWrite(PIN_REF_EN2, HIGH);
        s.ref2Enabled = true;
        if (verbose) {
            Serial.println(F("Reference clock set to REF2."));
        }
    } else {
        // ReferenceTarget::None — both clocks remain deasserted.
        if (verbose) {
            Serial.println(F("Warning: all reference clocks disabled — LOs will lose lock."));
        }
    }
}

void selectChipBinary(ChipTarget target)
{
    serialRxState.selectedBinaryTarget = target;
    selectChip(target);
}

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

void setSerialRxMode(SerialRxMode mode)
{
    serialRxState.mode = mode;
    serialRxState.wordLength = 0U;
}

void handleControlWord(uint32_t word)
{
    const uint16_t selector = static_cast<uint16_t>(word & 0xFFFFU);
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
    if (selector == 0x06FFU) {
        setSerialRxMode(SerialRxMode::AsciiLineData);
        return;
    }
    if (selector == 0x0EFFU) {
        setSerialRxMode(SerialRxMode::FMNData);
        return;
    }
    if (selector == 0x16FFU) {
        setSerialRxMode(SerialRxMode::Direct2Register);
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
        selectChipBinary(loTarget);
        return;
    }
    if (selector == 0x08FFU) {
        selectChipBinary(ChipTarget::Attenuator);
        return;
    }
    if (selector == 0x05FFU) {
        selectChipBinary(ChipTarget::ADC1);
        return;
    }
    if (selector == 0x0DFFU) {
        selectChipBinary(ChipTarget::ADC2);
        return;
    }
    if (selector == 0x15FFU) {
        selectChipBinary(ChipTarget::RAM);
        return;
    }
    if (selector == 0x1DFFU) {
        selectChipBinary(ChipTarget::Flash);
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

void handleFmnDataWord(uint32_t word)
{
    MAX2871* targetLo = nullptr;
    double* reportedFreq = nullptr;
    const ChipTarget target = serialRxState.selectedBinaryTarget;
    if (!loStateForTarget(target, &targetLo, &reportedFreq)) {
        return;
    }

    targetLo->setFrequency(word, targetLo->DIVA);
    *reportedFreq = targetLo->fmn2freq();
    markLoManual(target);
}

bool parseAsciiControlWord(const char* token, uint32_t* word)
{
    if (token == nullptr || word == nullptr) {
        return false;
    }
    *word = static_cast<uint32_t>(strtoul(token, nullptr, 16));
    return true;
}

void handleAttenuatorCommand(const char* valueToken)
{
    if (valueToken == nullptr) {
        return;
    }
    char* endPointer = nullptr;
    const double requestedDb = strtod(valueToken, &endPointer);
    if (endPointer == nullptr || *endPointer != '\0') {
        Serial.println(F("Invalid attenuator value."));
        return;
    }
    if (requestedDb < ATTEN_MIN_DB || requestedDb > ATTEN_MAX_DB) {
        Serial.println(F("Attenuator range is 0.0 to 31.75 dB."));
        return;
    }
    const double steps = (requestedDb - ATTEN_MIN_DB) / ATTEN_STEP_DB;
    const double roundedSteps = round(steps);
    if (fabs(steps - roundedSteps) > 0.01) {
        Serial.println(F("Attenuator step is 0.25 dB."));
        return;
    }

    programAttenuatorDb(requestedDb);
    Serial.print(F("Attenuator set to "));
    Serial.print(state.attenuatorDb, 2);
    Serial.println(F(" dB"));
    Serial.println(F("Reminder: expect ~51 ohms at 31.75 dB."));
    printStatus();
}

void handleIfmodeCommand(const char* modeToken)
{
    if (modeToken == nullptr) {
        Serial.println(F("Usage: ifmode <high|low>"));
        return;
    }
    const bool highRequested = equalsIgnoreCase(modeToken, "high");
    const bool lowRequested  = equalsIgnoreCase(modeToken, "low");
    if (!highRequested && !lowRequested) {
        Serial.println(F("ifmode requires 'high' or 'low'."));
        return;
    }
    if (state.chipTarget == ChipTarget::LO1) {
        Serial.println(F("LO1 injection mode is computed automatically from the frequency plan."));
        return;
    }
    if (state.chipTarget != ChipTarget::LO2 && state.chipTarget != ChipTarget::LO3) {
        Serial.println(F("Select lo2 or lo3 with 'chip' before using ifmode."));
        return;
    }
    // Reject ifmode while the selected LO is frozen under manual lofreq control.
    // Tune to a frequency first to restore automatic mode.
    if ((state.chipTarget == ChipTarget::LO2 && state.lo2Manual) ||
        (state.chipTarget == ChipTarget::LO3 && state.lo3Manual)) {
        Serial.print(chipTargetName(state.chipTarget));
        Serial.println(F(" is under manual lofreq control; ifmode has no effect."));
        Serial.println(F("Tune to a frequency (e.g. '1735.113') to restore automatic mode."));
        return;
    }
    const LOInjectionMode requestedMode = highRequested ? LOInjectionMode::High : LOInjectionMode::Low;
    switch (state.chipTarget) {
        case ChipTarget::LO2:
            state.desiredLo2Injection = requestedMode;
            break;
        case ChipTarget::LO3:
            state.desiredLo3Injection = requestedMode;
            break;
        default:
            return;
    }
    recomputePlan();
    Serial.print(F("IF mode updated for "));
    Serial.print(chipTargetName(state.chipTarget));
    Serial.print(F(" -> "));
    Serial.println(highRequested ? F("HIGH-side injection") : F("LOW-side injection"));
    printSelectedLoSnapshot(state.chipTarget);
    printInjectionSummary();
    Serial.println();
}

void handleLofreqCommand(const char* valueToken)
{
    if (valueToken == nullptr) {
        Serial.println(F("Usage: lofreq <MHz>"));
        return;
    }
    if (state.chipTarget != ChipTarget::LO1 &&
        state.chipTarget != ChipTarget::LO2 &&
        state.chipTarget != ChipTarget::LO3) {
        Serial.println(F("Select lo1, lo2, or lo3 with 'chip' before using lofreq."));
        return;
    }
    char* endPointer = nullptr;
    const double requestedMhz = strtod(valueToken, &endPointer);
    if (endPointer == nullptr || *endPointer != '\0' || !(requestedMhz > 0.0)) {
        Serial.println(F("lofreq requires a positive frequency in MHz."));
        return;
    }
    MAX2871* targetLo = nullptr;
    double* reportedFreq = nullptr;
    if (!loStateForTarget(state.chipTarget, &targetLo, &reportedFreq)) {
        return;
    }
    targetLo->setFrequency(requestedMhz);
    const double actual = targetLo->fmn2freq();
    *reportedFreq = actual;
    markLoManual(state.chipTarget);
    Serial.print(F("LO frequency set for "));
    Serial.print(chipTargetName(state.chipTarget));
    Serial.print(F(" -> "));
    Serial.print(actual, 3);
    Serial.println(F(" MHz"));
    printSelectedLoSnapshot(state.chipTarget);
    printInjectionSummary();
    Serial.println();
}

void handleChipCommand(const char* targetToken)
{
    if (targetToken == nullptr) {
        return;
    }
    ChipTarget target = ChipTarget::None;
    if (equalsIgnoreCase(targetToken, "lo1")) {
        target = ChipTarget::LO1;
    } else if (equalsIgnoreCase(targetToken, "lo2")) {
        target = ChipTarget::LO2;
    } else if (equalsIgnoreCase(targetToken, "lo3")) {
        target = ChipTarget::LO3;
    } else if (equalsIgnoreCase(targetToken, "atten")) {
        target = ChipTarget::Attenuator;
    } else if (equalsIgnoreCase(targetToken, "adc1")) {
        target = ChipTarget::ADC1;
    } else if (equalsIgnoreCase(targetToken, "adc2")) {
        target = ChipTarget::ADC2;
    } else if (equalsIgnoreCase(targetToken, "ram")) {
        target = ChipTarget::RAM;
    } else if (equalsIgnoreCase(targetToken, "flash")) {
        target = ChipTarget::Flash;
    } else if (equalsIgnoreCase(targetToken, "off")) {
        target = ChipTarget::None;
    } else {
        Serial.println(F("chip target must be lo1, lo2, lo3, atten, adc1, adc2, ram, flash, or off."));
        return;
    }
    selectChip(target);
}

void handleSetCommand(const char* targetToken)
{
    if (targetToken == nullptr) {
        Serial.println(F("Usage: set <ref1|ref2|off>"));
        return;
    }
    ReferenceTarget target = ReferenceTarget::None;
    if (equalsIgnoreCase(targetToken, "ref1")) {
        target = ReferenceTarget::Ref1;
    } else if (equalsIgnoreCase(targetToken, "ref2")) {
        target = ReferenceTarget::Ref2;
    } else if (equalsIgnoreCase(targetToken, "off")) {
        target = ReferenceTarget::None;
    } else {
        Serial.println(F("set target must be ref1, ref2, or off."));
        return;
    }
    selectRef(target);
}

void handleSpiCommand(const char* valueToken)
{
    if (valueToken == nullptr) {
        return;
    }
    if (state.chipTarget == ChipTarget::None) {
        Serial.println(F("No chip selected. Use 'chip <target>' first."));
        return;
    }
    char* endPointer = nullptr;
    const uint32_t value = static_cast<uint32_t>(strtoul(valueToken, &endPointer, 16));
    if (endPointer == nullptr || *endPointer != '\0') {
        Serial.println(F("SPI value must be hexadecimal (e.g., 0x12345678 or 12345678)."));
        return;
    }
    if (!state.manualSpiArmed) {
        if (!state.pendingSpiConfirmation) {
            state.pendingSpiConfirmation = true;
            state.pendingSpiValue        = value;
            state.pendingSpiTarget       = state.chipTarget;
            Serial.println(F("Manual SPI writes locked. Re-enter the same command to arm manual writes."));
            return;
        }
        if (state.pendingSpiValue != value || state.pendingSpiTarget != state.chipTarget) {
            state.pendingSpiValue  = value;
            state.pendingSpiTarget = state.chipTarget;
            Serial.println(F("Confirmation mismatch. Re-enter desired value to arm manual writes."));
            return;
        }
        state.pendingSpiConfirmation = false;
        state.manualSpiArmed         = true;
        Serial.println(F("Manual SPI writes armed. Proceed with caution."));
    }

    logManualWrite(value);

    switch (state.chipTarget) {
        case ChipTarget::LO1:
            halLo1.spiWriteRegister(value);
            break;
        case ChipTarget::LO2:
            halLo2.spiWriteRegister(value);
            break;
        case ChipTarget::LO3:
            halLo3.spiWriteRegister(value);
            break;
        case ChipTarget::Attenuator:
            programAttenuatorRaw(static_cast<uint8_t>(value & 0x7FU));
            break;
        case ChipTarget::ADC1:
            spiWrite32(PIN_ADC1, true, value);
            break;
        case ChipTarget::ADC2:
            spiWrite32(PIN_ADC2, true, value);
            break;
        case ChipTarget::RAM:
            spiWrite32(PIN_RAM, true, value);
            break;
        case ChipTarget::Flash:
            spiWrite32(PIN_FLASH, true, value);
            break;
        case ChipTarget::None:
        default:
            break;
    }
}

CommandKind commandKindFromToken(const char* token)
{
    if (equalsIgnoreCase(token, "help")) {
        return CommandKind::Help;
    }
    if (equalsIgnoreCase(token, "status")) {
        return CommandKind::Status;
    }
    if (equalsIgnoreCase(token, "relock")) {
        return CommandKind::Relock;
    }
    if (equalsIgnoreCase(token, "info")) {
        return CommandKind::Info;
    }
    if (equalsIgnoreCase(token, "atten")) {
        return CommandKind::Atten;
    }
    if (equalsIgnoreCase(token, "ifmode")) {
        return CommandKind::Ifmode;
    }
    if (equalsIgnoreCase(token, "lofreq")) {
        return CommandKind::Lofreq;
    }
    if (equalsIgnoreCase(token, "chip")) {
        return CommandKind::Chip;
    }
    if (equalsIgnoreCase(token, "set")) {
        return CommandKind::Set;
    }
    if (equalsIgnoreCase(token, "spi")) {
        return CommandKind::Spi;
    }
    return CommandKind::Unknown;
}

void handleCommand(const char* line)
{
    if (line == nullptr) {
        return;
    }
    char buffer[INPUT_BUFFER_SIZE];
    strncpy(buffer, line, sizeof(buffer) - 1U);
    buffer[sizeof(buffer) - 1U] = '\0';
    trimWhitespace(buffer);
    if (buffer[0] == '\0') {
        return;
    }

    char* tokens[4] = {nullptr, nullptr, nullptr, nullptr};
    size_t count = 0;
    char* token = strtok(buffer, " ");
    while (token != nullptr && count < (sizeof(tokens) / sizeof(tokens[0]))) {
        tokens[count++] = token;
        token = strtok(nullptr, " ");
    }

    if (tokens[0] == nullptr) {
        return;
    }

    uint32_t asciiControlWord = 0U;
    if (tokens[1] == nullptr && parseAsciiControlWord(tokens[0], &asciiControlWord)) {
        handleControlWord(asciiControlWord);
        return;
    }

    char* endPointer = nullptr;
    const double mhz = strtod(tokens[0], &endPointer);
    const bool parsedNumber = (endPointer != nullptr) && (*endPointer == '\0') && (tokens[1] == nullptr);
    if (parsedNumber) {
        if (mhz < MIN_RF_INPUT_MHZ || mhz > MAX_RF_INPUT_MHZ) {
            Serial.println(F("Frequency out of range (23.5 to 6000 MHz)."));
            return;
        }
        tuneTo(mhz);
        printStatus();
        return;
    }

    switch (commandKindFromToken(tokens[0])) {
        case CommandKind::Help:
            printBanner();
            return;
        case CommandKind::Status:
            printStatus();
            return;
        case CommandKind::Relock:
            initializeLo(lo1);
            initializeLo(lo2);
            initializeLo(lo3);
            tuneTo(currentRfInputMhz);
            Serial.println(F("MAX2871 devices reinitialized."));
            return;
        case CommandKind::Info:
            Serial.println(F("Pin assignments (Metro Mini):"));
            Serial.println(F("  chip targets (assert HIGH):"));
            Serial.println(F("    LO1 LE     -> A3"));
            Serial.println(F("    LO2 LE     -> D4"));
            Serial.println(F("    LO3 LE     -> A4"));
            Serial.println(F("    Atten CS   -> A5"));
            Serial.println(F("  chip targets (assert LOW):"));
            Serial.println(F("    ADC1 CS    -> see PIN_ADC1 in command_interface.h"));
            Serial.println(F("    ADC2 CS    -> see PIN_ADC2 in command_interface.h"));
            Serial.println(F("    RAM  CS    -> see PIN_RAM  in command_interface.h"));
            Serial.println(F("    Flash CS   -> see PIN_FLASH in command_interface.h"));
            Serial.println(F("  set targets (assert HIGH):"));
            Serial.println(F("    REF_EN1    -> D5"));
            Serial.println(F("    REF_EN2    -> D6"));
            return;
        case CommandKind::Atten:
            if (count < 2U) {
                Serial.println(F("Usage: atten <dB>"));
                return;
            }
            handleAttenuatorCommand(tokens[1]);
            return;
        case CommandKind::Ifmode:
            if (count < 2U) {
                Serial.println(F("Usage: ifmode <high|low>"));
                return;
            }
            handleIfmodeCommand(tokens[1]);
            return;
        case CommandKind::Lofreq:
            if (count < 2U) {
                Serial.println(F("Usage: lofreq <MHz>"));
                return;
            }
            handleLofreqCommand(tokens[1]);
            return;
        case CommandKind::Chip:
            if (count < 2U) {
                Serial.println(F("Usage: chip <lo1|lo2|lo3|atten|adc1|adc2|ram|flash|off>"));
                return;
            }
            handleChipCommand(tokens[1]);
            return;
        case CommandKind::Set:
            if (count < 2U) {
                Serial.println(F("Usage: set <ref1|ref2|off>"));
                return;
            }
            handleSetCommand(tokens[1]);
            return;
        case CommandKind::Spi:
            if (count < 2U) {
                Serial.println(F("Usage: spi <hex32>"));
                return;
            }
            handleSpiCommand(tokens[1]);
            return;
        case CommandKind::Unknown:
        default:
            Serial.print(F("Unknown command: "));
            Serial.println(tokens[0]);
            Serial.println(F("Type 'help' for a list of commands."));
            return;
    }
}
} // namespace
