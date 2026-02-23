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

bool isRefTarget(ChipTarget target)
{
    return (target == ChipTarget::Ref1) || (target == ChipTarget::Ref2);
}

bool isSpiPeripheral(ChipTarget target)
{
    switch (target) {
        case ChipTarget::LO1:
        case ChipTarget::LO2:
        case ChipTarget::LO3:
        case ChipTarget::Attenuator:
        case ChipTarget::ADC1:
        case ChipTarget::ADC2:
        case ChipTarget::RAM:
        case ChipTarget::Flash:
            return true;
        default:
            return false;
    }
}

bool hasHardwareChipSelect(ChipTarget target)
{
    switch (target) {
        case ChipTarget::LO1:
        case ChipTarget::LO2:
        case ChipTarget::LO3:
        case ChipTarget::Attenuator:
        case ChipTarget::Ref1:
        case ChipTarget::Ref2:
            return true;
        default:
            return false;
    }
}

void ensureRefSelection(ChipTarget target)
{
    if (target == ChipTarget::Ref1) {
        if (state.ref2Enabled) {
#if !defined(SPECANN_CI_BUILD)
            digitalWrite(PIN_REF_EN2, LOW);
#endif
            state.ref2Enabled = false;
        }
        if (!state.ref1Enabled) {
#if !defined(SPECANN_CI_BUILD)
            digitalWrite(PIN_REF_EN1, HIGH);
#endif
            state.ref1Enabled = true;
        }
    } else if (target == ChipTarget::Ref2) {
        if (state.ref1Enabled) {
#if !defined(SPECANN_CI_BUILD)
            digitalWrite(PIN_REF_EN1, LOW);
#endif
            state.ref1Enabled = false;
        }
        if (!state.ref2Enabled) {
#if !defined(SPECANN_CI_BUILD)
            digitalWrite(PIN_REF_EN2, HIGH);
#endif
            state.ref2Enabled = true;
        }
    }
}

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

void deassertTargetInternal(ChipTarget target)
{
    switch (target) {
        case ChipTarget::LO1:
            digitalWrite(PIN_LE_LO1, HIGH);
            break;
        case ChipTarget::LO2:
            digitalWrite(PIN_LE_LO2, HIGH);
            break;
        case ChipTarget::LO3:
            digitalWrite(PIN_LE_LO3, HIGH);
            break;
        case ChipTarget::Attenuator:
            digitalWrite(PIN_ATTEN, HIGH);
            break;
        case ChipTarget::Ref1:
        case ChipTarget::Ref2:
        case ChipTarget::ADC1:
        case ChipTarget::ADC2:
        case ChipTarget::RAM:
        case ChipTarget::Flash:
        case ChipTarget::None:
        default:
            break;
    }
}

void programAttenuatorRaw(uint8_t code)
{
#if !defined(SPECANN_CI_BUILD)
    deassertTargetInternal(ChipTarget::Attenuator);
    SPI.beginTransaction(SPISettings(ATTEN_SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_ATTEN, LOW);
    SPI.transfer(code);
    digitalWrite(PIN_ATTEN, HIGH);
    SPI.endTransaction();
#else
    (void)code;
    Serial.println(F("(CI) Attenuator write skipped."));
#endif
    const double mappedDb = ATTEN_MIN_DB + (static_cast<double>(code) * ATTEN_STEP_DB);
    if (mappedDb >= ATTEN_MIN_DB && mappedDb <= (ATTEN_MAX_DB + 0.25)) {
        state.attenuatorDb = mappedDb;
    }
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
    Serial.println(F("WARNING: manual writes can damage hardware if misused."));
}

void selectChip(ChipTarget target)
{
    if (target == state.chipTarget) {
        Serial.print(F("Manual target unchanged: "));
        Serial.println(chipTargetName(state.chipTarget));
        return;
    }
    deassertTargetInternal(state.chipTarget);
    state.chipTarget = target;
    state.manualSpiArmed = false;
    state.pendingSpiConfirmation = false;
    if (isRefTarget(target)) {
        ensureRefSelection(target);
        Serial.print(F("Reference select set to "));
        Serial.println(chipTargetName(state.chipTarget));
        return;
    }
    Serial.print(F("Manual target set to "));
    Serial.println(chipTargetName(state.chipTarget));
    if (!hasHardwareChipSelect(target) && isSpiPeripheral(target)) {
        Serial.println(F("Note: chip-select control not wired; SPI writes will only be logged."));
    }
}

void handleAttenuatorCommand(const char* valueToken);
void handleIfmodeCommand(const char* modeToken);
void handleLofreqCommand(const char* valueToken);
void handleChipCommand(const char* targetToken);
void handleSpiCommand(const char* valueToken);
void handleCommand(const char* line);

} // namespace

extern ArduinoHAL halLo1;
extern ArduinoHAL halLo2;
extern ArduinoHAL halLo3;
extern MAX2871 lo1;
extern MAX2871 lo2;
extern MAX2871 lo3;
extern FrequencyCalculator freqCalc;
extern double currentRfInputMhz;
extern LOInjectionMode desiredLo1Injection;
extern LOInjectionMode desiredLo2Injection;
extern LOInjectionMode desiredLo3Injection;

void tuneTo(double mhz);
void printStatus();
void initializeLo(MAX2871& lo);
void recomputePlan();

void printBanner()
{
    Serial.println();
    Serial.println(F("=== SpecAnn Technician Console ==="));
    Serial.println(F("Commands:"));
    Serial.println(F("  <MHz>              Tune synthesizers (23.5 to 6000 MHz)"));
    Serial.println(F("  help               Show this list"));
    Serial.println(F("  status             Report LO/IF plan, attenuator state, chip target"));
    Serial.println(F("  relock             Reinitialize MAX2871 devices"));
    Serial.println(F("  info               Show board pin assignments"));
    Serial.println(F("  atten <dB>         Program PE43711 attenuator (1.0 to 31.75 dB in 0.25 steps)"));
    Serial.println(F("  ifmode <high|low>  Set injection for the selected LO (use chip first)"));
    Serial.println(F("  lofreq <MHz>       Program the selected LO directly"));
    Serial.println(F("  chip <lo1|lo2|lo3|atten|ref1|ref2|adc1|adc2|ram|flash>  Select bus target"));
    Serial.println(F("  spi <hex32>        Send raw 32-bit word to selected device"));
    Serial.println();
}

void pollSerial()
{
    while (Serial.available() > 0) {
        const char incoming = static_cast<char>(Serial.read());
        if (incoming == '\r') {
            continue;
        }
        if (incoming == '\n') {
            inputBuffer[inputLength] = '\0';
            handleCommand(inputBuffer);
            inputLength = 0;
        } else if (inputLength < (INPUT_BUFFER_SIZE - 1U)) {
            inputBuffer[inputLength++] = incoming;
        } else {
            inputLength = 0;
            Serial.println(F("Input too long, line cleared."));
        }
    }
}

void programAttenuatorDb(double db)
{
    const uint8_t code = attenCodeFromDb(db);
    programAttenuatorRaw(code);
    state.attenuatorDb = ATTEN_MIN_DB + (static_cast<double>(code) * ATTEN_STEP_DB);
}

double getCurrentAttenuatorDb()
{
    return state.attenuatorDb;
}

ChipTarget getCurrentChipTarget()
{
    return state.chipTarget;
}

const __FlashStringHelper* chipTargetName(ChipTarget target)
{
    switch (target) {
        case ChipTarget::LO1: return F("LO1");
        case ChipTarget::LO2: return F("LO2");
        case ChipTarget::LO3: return F("LO3");
        case ChipTarget::Attenuator: return F("Attenuator");
        case ChipTarget::Ref1: return F("REF1");
        case ChipTarget::Ref2: return F("REF2");
        case ChipTarget::ADC1: return F("ADC1");
        case ChipTarget::ADC2: return F("ADC2");
        case ChipTarget::RAM: return F("RAM");
        case ChipTarget::Flash: return F("FLASH");
        case ChipTarget::None:
        default: return F("None");
    }
}

namespace {

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
        Serial.println(F("Attenuator range is 1.0 to 31.75 dB."));
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
    const bool lowRequested = equalsIgnoreCase(modeToken, "low");
    if (!highRequested && !lowRequested) {
        Serial.println(F("ifmode requires 'high' or 'low'."));
        return;
    }
    if (state.chipTarget != ChipTarget::LO1 && state.chipTarget != ChipTarget::LO2 && state.chipTarget != ChipTarget::LO3) {
        Serial.println(F("Select lo1, lo2, or lo3 with 'chip' before using ifmode."));
        return;
    }
    const LOInjectionMode requestedMode = highRequested ? LOInjectionMode::High : LOInjectionMode::Low;
    LOInjectionMode* injectionPtr = nullptr;
    switch (state.chipTarget) {
        case ChipTarget::LO1:
            injectionPtr = &desiredLo1Injection;
            break;
        case ChipTarget::LO2:
            injectionPtr = &desiredLo2Injection;
            break;
        case ChipTarget::LO3:
            injectionPtr = &desiredLo3Injection;
            break;
        default:
            break;
    }
    if (injectionPtr == nullptr) {
        return;
    }
    *injectionPtr = requestedMode;
    recomputePlan();
    Serial.print(F("IF mode updated for "));
    Serial.print(chipTargetName(state.chipTarget));
    Serial.print(F(" -> "));
    Serial.println(highRequested ? F("HIGH-side injection") : F("LOW-side injection"));
    printStatus();
}

void handleLofreqCommand(const char* valueToken)
{
    if (valueToken == nullptr) {
        Serial.println(F("Usage: lofreq <MHz>"));
        return;
    }
    if (state.chipTarget != ChipTarget::LO1 && state.chipTarget != ChipTarget::LO2 && state.chipTarget != ChipTarget::LO3) {
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
    switch (state.chipTarget) {
        case ChipTarget::LO1:
            targetLo = &lo1;
            reportedFreq = &freqCalc.FreqLO1;
            break;
        case ChipTarget::LO2:
            targetLo = &lo2;
            reportedFreq = &freqCalc.FreqLO2;
            break;
        case ChipTarget::LO3:
            targetLo = &lo3;
            reportedFreq = &freqCalc.FreqLO3;
            break;
        default:
            break;
    }
    if (targetLo == nullptr || reportedFreq == nullptr) {
        return;
    }
#if !defined(SPECANN_CI_BUILD)
    targetLo->setFrequency(requestedMhz);
    const double actual = targetLo->fmn2freq();
#else
    const double actual = requestedMhz;
#endif
    *reportedFreq = actual;
    Serial.print(F("LO frequency set for "));
    Serial.print(chipTargetName(state.chipTarget));
    Serial.print(F(" -> "));
    Serial.print(actual, 3);
    Serial.println(F(" MHz"));
    printStatus();
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
    } else if (equalsIgnoreCase(targetToken, "ref1")) {
        target = ChipTarget::Ref1;
    } else if (equalsIgnoreCase(targetToken, "ref2")) {
        target = ChipTarget::Ref2;
    } else if (equalsIgnoreCase(targetToken, "adc1")) {
        target = ChipTarget::ADC1;
    } else if (equalsIgnoreCase(targetToken, "adc2")) {
        target = ChipTarget::ADC2;
    } else if (equalsIgnoreCase(targetToken, "ram")) {
        target = ChipTarget::RAM;
    } else if (equalsIgnoreCase(targetToken, "flash")) {
        target = ChipTarget::Flash;
    } else {
        Serial.println(F("chip target must be lo1, lo2, lo3, atten, ref1, ref2, adc1, adc2, ram, or flash."));
        return;
    }
    selectChip(target);
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
            state.pendingSpiValue = value;
            state.pendingSpiTarget = state.chipTarget;
            Serial.println(F("Manual SPI writes locked. Re-enter the same command to arm manual writes."));
            return;
        }
        if (state.pendingSpiValue != value || state.pendingSpiTarget != state.chipTarget) {
            state.pendingSpiValue = value;
            state.pendingSpiTarget = state.chipTarget;
            Serial.println(F("Confirmation mismatch. Re-enter desired value to arm manual writes."));
            return;
        }
        state.pendingSpiConfirmation = false;
        state.manualSpiArmed = true;
        Serial.println(F("Manual SPI writes armed. Proceed with caution."));
    }

    logManualWrite(value);

    switch (state.chipTarget) {
        case ChipTarget::LO1:
#if !defined(SPECANN_CI_BUILD)
            halLo1.spiWriteRegister(value);
#else
            Serial.println(F("(CI) LO1 write skipped."));
#endif
            break;
        case ChipTarget::LO2:
#if !defined(SPECANN_CI_BUILD)
            halLo2.spiWriteRegister(value);
#else
            Serial.println(F("(CI) LO2 write skipped."));
#endif
            break;
        case ChipTarget::LO3:
#if !defined(SPECANN_CI_BUILD)
            halLo3.spiWriteRegister(value);
#else
            Serial.println(F("(CI) LO3 write skipped."));
#endif
            break;
        case ChipTarget::Attenuator:
            programAttenuatorRaw(static_cast<uint8_t>(value & 0x7FU));
            break;
        case ChipTarget::Ref1:
        case ChipTarget::Ref2:
            Serial.println(F("Reference selects are not on the SPI bus; write skipped."));
            break;
        case ChipTarget::ADC1:
        case ChipTarget::ADC2:
        case ChipTarget::RAM:
        case ChipTarget::Flash:
            Serial.println(F("SPI control for the selected target is not wired; write skipped."));
            break;
        case ChipTarget::None:
        default:
            break;
    }
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

    if (equalsIgnoreCase(tokens[0], "help")) {
        printBanner();
        return;
    }
    if (equalsIgnoreCase(tokens[0], "status")) {
        printStatus();
        return;
    }
    if (equalsIgnoreCase(tokens[0], "relock")) {
        initializeLo(lo1);
        initializeLo(lo2);
        initializeLo(lo3);
        tuneTo(currentRfInputMhz);
        Serial.println(F("MAX2871 devices reinitialized."));
        return;
    }
    if (equalsIgnoreCase(tokens[0], "info")) {
        Serial.println(F("Pin assignments (Metro Mini):"));
        Serial.println(F("  LO1 LE -> A3"));
        Serial.println(F("  LO2 LE -> D4"));
        Serial.println(F("  LO3 LE -> A4"));
        Serial.println(F("  Attenuator CS -> A5"));
        Serial.println(F("  REF_EN1 -> D5 (HIGH to enable)"));
        Serial.println(F("  REF_EN2 -> D6 (LOW default)"));
        Serial.println(F("  Status pin -> D10 (500 Hz heartbeat)"));
        return;
    }
    if (equalsIgnoreCase(tokens[0], "atten")) {
        if (count < 2U) {
            Serial.println(F("Usage: atten <dB>"));
            return;
        }
        handleAttenuatorCommand(tokens[1]);
        return;
    }
    if (equalsIgnoreCase(tokens[0], "ifmode")) {
        if (count < 2U) {
            Serial.println(F("Usage: ifmode <high|low>"));
            return;
        }
        handleIfmodeCommand(tokens[1]);
        return;
    }
    if (equalsIgnoreCase(tokens[0], "lofreq")) {
        if (count < 2U) {
            Serial.println(F("Usage: lofreq <MHz>"));
            return;
        }
        handleLofreqCommand(tokens[1]);
        return;
    }
    if (equalsIgnoreCase(tokens[0], "chip")) {
        if (count < 2U) {
            Serial.println(F("Usage: chip <lo1|lo2|lo3|atten|ref1|ref2|adc1|adc2|ram|flash>"));
            return;
        }
        handleChipCommand(tokens[1]);
        return;
    }
    if (equalsIgnoreCase(tokens[0], "spi")) {
        if (count < 2U) {
            Serial.println(F("Usage: spi <hex32>"));
            return;
        }
        handleSpiCommand(tokens[1]);
        return;
    }

    Serial.print(F("Unknown command: "));
    Serial.println(tokens[0]);
    Serial.println(F("Type 'help' for a list of commands."));
}
} // namespace
