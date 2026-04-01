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

} // namespace

extern ArduinoHAL halLo1;
extern ArduinoHAL halLo2;
extern ArduinoHAL halLo3;
extern MAX2871 lo1;
extern MAX2871 lo2;
extern MAX2871 lo3;
extern FrequencyCalculator freqCalc;
extern double currentRfInputMhz;

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
    // Deassert all CS/LE pins to their idle levels.
    // LO1/LO2/LO3/Attenuator assert HIGH  → idle LOW.
    // ADC1/ADC2/RAM/Flash      assert LOW  → idle HIGH.
    digitalWrite(PIN_LE_LO1, LOW);
    digitalWrite(PIN_LE_LO2, LOW);
    digitalWrite(PIN_LE_LO3, LOW);
    digitalWrite(PIN_ATTEN,  LOW);
    digitalWrite(PIN_ADC1,   HIGH);
    digitalWrite(PIN_ADC2,   HIGH);
    digitalWrite(PIN_RAM,    HIGH);
    digitalWrite(PIN_FLASH,  HIGH);

    // Assert the requested target.
    switch (target) {
        case ChipTarget::LO1:        digitalWrite(PIN_LE_LO1, HIGH); break;
        case ChipTarget::LO2:        digitalWrite(PIN_LE_LO2, HIGH); break;
        case ChipTarget::LO3:        digitalWrite(PIN_LE_LO3, HIGH); break;
        case ChipTarget::Attenuator: digitalWrite(PIN_ATTEN,  HIGH); break;
        case ChipTarget::ADC1:       digitalWrite(PIN_ADC1,   LOW);  break;
        case ChipTarget::ADC2:       digitalWrite(PIN_ADC2,   LOW);  break;
        case ChipTarget::RAM:        digitalWrite(PIN_RAM,    LOW);  break;
        case ChipTarget::Flash:      digitalWrite(PIN_FLASH,  LOW);  break;
        case ChipTarget::None:
        default:
            break;
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
void selectRef(ChipTarget target)
{
    if (target != ChipTarget::Ref1 &&
        target != ChipTarget::Ref2 &&
        target != ChipTarget::None) {
        Serial.println(F("set requires ref1, ref2, or off."));
        return;
    }

    ConsoleState& s = consoleState();

    // Deassert both reference clocks unconditionally.
    digitalWrite(PIN_REF_EN1, LOW);
    digitalWrite(PIN_REF_EN2, LOW);

    s.ref1Enabled = false;
    s.ref2Enabled = false;

    if (target == ChipTarget::Ref1) {
        digitalWrite(PIN_REF_EN1, HIGH);
        s.ref1Enabled = true;
        Serial.println(F("Reference clock set to REF1."));
    } else if (target == ChipTarget::Ref2) {
        digitalWrite(PIN_REF_EN2, HIGH);
        s.ref2Enabled = true;
        Serial.println(F("Reference clock set to REF2."));
    } else {
        // ChipTarget::None — both clocks remain deasserted.
        Serial.println(F("Warning: all reference clocks disabled — LOs will lose lock."));
    }
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

// cppcheck-suppress unusedFunction
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
        case ChipTarget::Ref1:       return F("REF1");
        case ChipTarget::Ref2:       return F("REF2");
        case ChipTarget::ADC1:       return F("ADC1");
        case ChipTarget::ADC2:       return F("ADC2");
        case ChipTarget::RAM:        return F("RAM");
        case ChipTarget::Flash:      return F("FLASH");
        case ChipTarget::None:
        default:                     return F("None");
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
    MAX2871* targetLo       = nullptr;
    double* reportedFreq    = nullptr;
    switch (state.chipTarget) {
        case ChipTarget::LO1:
            targetLo     = &lo1;
            reportedFreq = &freqCalc.FreqLO1;
            break;
        case ChipTarget::LO2:
            targetLo     = &lo2;
            reportedFreq = &freqCalc.FreqLO2;
            break;
        case ChipTarget::LO3:
            targetLo     = &lo3;
            reportedFreq = &freqCalc.FreqLO3;
            break;
        default:
            break;
    }
    if (targetLo == nullptr || reportedFreq == nullptr) {
        return;
    }
    targetLo->setFrequency(requestedMhz);
    const double actual = targetLo->fmn2freq();
    *reportedFreq = actual;
    // Mark this LO as manually controlled; recomputePlan will preserve its frequency.
    switch (state.chipTarget) {
        case ChipTarget::LO1: state.lo1Manual = true; break;
        case ChipTarget::LO2: state.lo2Manual = true; break;
        case ChipTarget::LO3: state.lo3Manual = true; break;
        default: break;
    }
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
    ChipTarget target = ChipTarget::None;
    if (equalsIgnoreCase(targetToken, "ref1")) {
        target = ChipTarget::Ref1;
    } else if (equalsIgnoreCase(targetToken, "ref2")) {
        target = ChipTarget::Ref2;
    } else if (equalsIgnoreCase(targetToken, "off")) {
        target = ChipTarget::None;
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
        Serial.println(F("  Status pin  -> D10 (500 Hz heartbeat)"));
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
            Serial.println(F("Usage: chip <lo1|lo2|lo3|atten|adc1|adc2|ram|flash|off>"));
            return;
        }
        handleChipCommand(tokens[1]);
        return;
    }
    if (equalsIgnoreCase(tokens[0], "set")) {
        if (count < 2U) {
            Serial.println(F("Usage: set <ref1|ref2|off>"));
            return;
        }
        handleSetCommand(tokens[1]);
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