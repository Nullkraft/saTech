#include "technician_console.h"

#include "command_interface.h"
#include "console_state.h"

#include <arduino_hal.h>
#include <ctype.h>
#include <frequency_calculator.h>
#include <math.h>
#include <max2871.h>
#include <stdlib.h>
#include <string.h>

extern MAX2871 lo1;
extern MAX2871 lo2;
extern MAX2871 lo3;
extern FrequencyCalculator freqCalc;

void printStatus();
void recomputePlan();
void tuneTo(double mhz);

namespace {

char technicianInputBuffer[INPUT_BUFFER_SIZE];
size_t technicianInputLength = 0U;

enum class TechnicianCommandKind {
    Unknown,
    Help,
    Status,
    Relock,
    Info,
    Rfin,
    Atten,
    Ifmode,
    Lofreq,
    Chip,
    Set,
    Spi,
};

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

const __FlashStringHelper* injectionLabel(LOInjectionMode mode)
{
    return (mode == LOInjectionMode::High) ? F("High") : F("Low");
}

uint8_t attenCodeFromDb(double db)
{
    const double steps = (db - ATTEN_MIN_DB) / ATTEN_STEP_DB;
    return static_cast<uint8_t>(lround(steps));
}

bool parseControlWord(const char* token, uint32_t* word)
{
    if (token == nullptr || word == nullptr) {
        return false;
    }
    char* endPointer = nullptr;
    *word = static_cast<uint32_t>(strtoul(token, &endPointer, 16));
    return endPointer != token && *endPointer == '\0';
}

void logManualWrite(uint32_t value)
{
    Serial.print(F("[SPI] target="));
    Serial.print(chipTargetName(getCurrentChipTarget()));
    Serial.print(F(" value=0x"));
    static const char hexDigits[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4) {
        const uint8_t nibble = static_cast<uint8_t>((value >> shift) & 0x0FU);
        Serial.print(hexDigits[nibble]);
    }
    Serial.println();
}

void markLoManual(ChipTarget target)
{
    ConsoleState& state = consoleState();
    switch (target) {
        case ChipTarget::LO1: state.lo1Manual = true; break;
        case ChipTarget::LO2: state.lo2Manual = true; break;
        case ChipTarget::LO3: state.lo3Manual = true; break;
        default: break;
    }
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

void processAttenuatorToken(const char* valueToken)
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

    const uint8_t code = attenCodeFromDb(requestedDb);
    processReceivedWord(0x000008FFUL | (static_cast<uint32_t>(code) << 16));
    Serial.print(F("Attenuator set to "));
    Serial.print(getCurrentAttenuatorDb(), 2);
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
    ConsoleState& state = consoleState();
    if (state.chipTarget == ChipTarget::LO1) {
        Serial.println(F("LO1 injection mode is computed automatically from the frequency plan."));
        return;
    }
    if (state.chipTarget != ChipTarget::LO2 && state.chipTarget != ChipTarget::LO3) {
        Serial.println(F("Select lo2 or lo3 with 'chip' before using ifmode."));
        return;
    }
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
    const ChipTarget chipTarget = getCurrentChipTarget();
    if (chipTarget != ChipTarget::LO1 &&
        chipTarget != ChipTarget::LO2 &&
        chipTarget != ChipTarget::LO3) {
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
    if (!loStateForTarget(chipTarget, &targetLo, &reportedFreq)) {
        return;
    }
    targetLo->setFrequency(requestedMhz);
    const double actual = targetLo->fmn2freq();
    *reportedFreq = actual;
    markLoManual(chipTarget);
    Serial.print(F("LO frequency set for "));
    Serial.print(chipTargetName(chipTarget));
    Serial.print(F(" -> "));
    Serial.print(actual, 3);
    Serial.println(F(" MHz"));
    printSelectedLoSnapshot(chipTarget);
    printInjectionSummary();
    Serial.println();
}

bool chipSelectorForToken(const char* targetToken, uint16_t* selector)
{
    if (targetToken == nullptr || selector == nullptr) {
        return false;
    }
    if (equalsIgnoreCase(targetToken, "lo1")) {
        *selector = 0x01FFU;
        return true;
    } else if (equalsIgnoreCase(targetToken, "lo2")) {
        *selector = 0x02FFU;
        return true;
    } else if (equalsIgnoreCase(targetToken, "lo3")) {
        *selector = 0x03FFU;
        return true;
    } else if (equalsIgnoreCase(targetToken, "adc1")) {
        *selector = 0x05FFU;
        return true;
    } else if (equalsIgnoreCase(targetToken, "adc2")) {
        *selector = 0x0DFFU;
        return true;
    } else if (equalsIgnoreCase(targetToken, "ram")) {
        *selector = 0x15FFU;
        return true;
    } else if (equalsIgnoreCase(targetToken, "flash")) {
        *selector = 0x1DFFU;
        return true;
    }
    return false;
}

void processChipToken(const char* targetToken)
{
    if (targetToken == nullptr) {
        return;
    }
    uint16_t selector = 0U;
    if (chipSelectorForToken(targetToken, &selector)) {
        processReceivedWord(static_cast<uint32_t>(selector));
        return;
    }
    if (equalsIgnoreCase(targetToken, "atten")) {
        selectSerialChipTarget(ChipTarget::Attenuator);
        return;
    }
    if (equalsIgnoreCase(targetToken, "off")) {
        selectSerialChipTarget(ChipTarget::None);
        return;
    }
    Serial.println(F("chip target must be lo1, lo2, lo3, atten, adc1, adc2, ram, flash, or off."));
}

bool referenceSelectorForToken(const char* targetToken, uint16_t* selector)
{
    if (targetToken == nullptr || selector == nullptr) {
        return false;
    }
    if (equalsIgnoreCase(targetToken, "ref1")) {
        *selector = 0x0CFFU;
        return true;
    } else if (equalsIgnoreCase(targetToken, "ref2")) {
        *selector = 0x14FFU;
        return true;
    } else if (equalsIgnoreCase(targetToken, "off")) {
        *selector = 0x04FFU;
        return true;
    }
    return false;
}

void processSetToken(const char* targetToken)
{
    if (targetToken == nullptr) {
        Serial.println(F("Usage: set <ref1|ref2|off>"));
        return;
    }
    uint16_t selector = 0U;
    if (!referenceSelectorForToken(targetToken, &selector)) {
        Serial.println(F("set target must be ref1, ref2, or off."));
        return;
    }
    processReceivedWord(static_cast<uint32_t>(selector));
    if (selector == 0x0CFFU) {
        Serial.println(F("Reference clock set to REF1."));
    } else if (selector == 0x14FFU) {
        Serial.println(F("Reference clock set to REF2."));
    } else {
        Serial.println(F("Warning: all reference clocks disabled - LOs will lose lock."));
    }
}

void processSpiToken(const char* valueToken)
{
    if (valueToken == nullptr) {
        return;
    }
    ConsoleState& state = consoleState();
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
    processDirectRegisterData(value);
}

TechnicianCommandKind commandKindFromToken(const char* token)
{
    if (equalsIgnoreCase(token, "help")) {
        return TechnicianCommandKind::Help;
    }
    if (equalsIgnoreCase(token, "status")) {
        return TechnicianCommandKind::Status;
    }
    if (equalsIgnoreCase(token, "relock")) {
        return TechnicianCommandKind::Relock;
    }
    if (equalsIgnoreCase(token, "info")) {
        return TechnicianCommandKind::Info;
    }
    if (equalsIgnoreCase(token, "rfin")) {
        return TechnicianCommandKind::Rfin;
    }
    if (equalsIgnoreCase(token, "atten")) {
        return TechnicianCommandKind::Atten;
    }
    if (equalsIgnoreCase(token, "ifmode")) {
        return TechnicianCommandKind::Ifmode;
    }
    if (equalsIgnoreCase(token, "lofreq")) {
        return TechnicianCommandKind::Lofreq;
    }
    if (equalsIgnoreCase(token, "chip")) {
        return TechnicianCommandKind::Chip;
    }
    if (equalsIgnoreCase(token, "set")) {
        return TechnicianCommandKind::Set;
    }
    if (equalsIgnoreCase(token, "spi")) {
        return TechnicianCommandKind::Spi;
    }
    return TechnicianCommandKind::Unknown;
}

} // namespace

void printInjectionSummary()
{
    const ConsoleState& state = consoleState();
    Serial.print(F("Injection: LO1="));
    Serial.print(state.lo1Manual ? F("Manual") : injectionLabel(freqCalc.LO1InjectionMode));
    Serial.print(F(" LO2="));
    Serial.print(state.lo2Manual ? F("Manual") : injectionLabel(freqCalc.LO2InjectionMode));
    Serial.print(F(" LO3="));
    Serial.println(state.lo3Manual ? F("Manual") : injectionLabel(freqCalc.LO3InjectionMode));
}

void printTechnicianBanner()
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

void handleTechnicianCommand(const char* line)
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

    if (tokens[1] == nullptr) {
        if (equalsIgnoreCase(tokens[0], "ascii") || equalsIgnoreCase(tokens[0], "binary")) {
            saTech.begin(tokens[0]);
            return;
        }
        uint32_t controlWord = 0U;
        if (parseControlWord(tokens[0], &controlWord)) {
            processReceivedWord(controlWord);
            return;
        }
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
        case TechnicianCommandKind::Help:
            printTechnicianBanner();
            return;
        case TechnicianCommandKind::Status:
            printStatus();
            return;
        case TechnicianCommandKind::Relock:
            processReceivedWord(0x00002FFFUL);
            return;
        case TechnicianCommandKind::Info:
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
        case TechnicianCommandKind::Rfin: {
            if (count < 2U) {
                Serial.println(F("Usage: RFin <MHz>"));
                return;
            }
            char* rfinEndPointer;
            const double rfinMhz = strtod(tokens[1], &rfinEndPointer);
            if (rfinEndPointer == tokens[1] || *rfinEndPointer != '\0') {
                Serial.println(F("Usage: RFin <MHz>"));
                return;
            }
            if (rfinMhz < MIN_RF_INPUT_MHZ || rfinMhz > MAX_RF_INPUT_MHZ) {
                Serial.println(F("Frequency out of range (23.5 to 6000 MHz)."));
                return;
            }
            tuneTo(rfinMhz);
            printStatus();
            return;
        }
        case TechnicianCommandKind::Atten:
            if (count < 2U) {
                Serial.println(F("Usage: atten <dB>"));
                return;
            }
            processAttenuatorToken(tokens[1]);
            return;
        case TechnicianCommandKind::Ifmode:
            if (count < 2U) {
                Serial.println(F("Usage: ifmode <high|low>"));
                return;
            }
            handleIfmodeCommand(tokens[1]);
            return;
        case TechnicianCommandKind::Lofreq:
            if (count < 2U) {
                Serial.println(F("Usage: lofreq <MHz>"));
                return;
            }
            handleLofreqCommand(tokens[1]);
            return;
        case TechnicianCommandKind::Chip:
            if (count < 2U) {
                Serial.println(F("Usage: chip <lo1|lo2|lo3|atten|adc1|adc2|ram|flash|off>"));
                return;
            }
            processChipToken(tokens[1]);
            return;
        case TechnicianCommandKind::Set:
            if (count < 2U) {
                Serial.println(F("Usage: set <ref1|ref2|off>"));
                return;
            }
            processSetToken(tokens[1]);
            return;
        case TechnicianCommandKind::Spi:
            if (count < 2U) {
                Serial.println(F("Usage: spi <hex32>"));
                return;
            }
            processSpiToken(tokens[1]);
            return;
        case TechnicianCommandKind::Unknown:
        default:
            Serial.print(F("Unknown command: "));
            Serial.println(tokens[0]);
            Serial.println(F("Type 'help' for a list of commands."));
            return;
    }
}

// cppcheck-suppress unusedFunction
void pollTechnicianConsole()
{
    while (Serial.available() > 0) {
        const char incomingChar = static_cast<char>(Serial.read());
        const uint8_t incomingByte = static_cast<uint8_t>(incomingChar);
        if (incomingChar != '\r' && incomingChar != '\n' && isprint(incomingByte) == 0) {
            continue;
        }
        if (incomingChar == '\r') {
            continue;
        }
        if (incomingChar == '\n') {
            technicianInputBuffer[technicianInputLength] = '\0';
            handleTechnicianCommand(technicianInputBuffer);
            technicianInputLength = 0U;
            continue;
        }
        if (technicianInputLength < (INPUT_BUFFER_SIZE - 1U)) {
            technicianInputBuffer[technicianInputLength++] = incomingChar;
            continue;
        }
        technicianInputLength = 0U;
        Serial.println(F("Input too long, line cleared."));
    }
}
