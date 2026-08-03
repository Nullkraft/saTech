#include "technician_console.h"

#include "board_devices.h"
#include "command_interface.h"
#include "console_state.h"
#include "fulltest.h"

#include <ctype.h>
#include <avr/pgmspace.h>
#include <stdlib.h>
#include <string.h>

namespace {

bool loStateForTarget(ChipTarget target, MAX2871** targetLo, double** reportedFreq);

char lineInputBuffer[INPUT_BUFFER_SIZE];
size_t bufferIndex = 0U;

constexpr double ANALYZER_RF_INPUT_MIN_MHZ = 0.0;
constexpr double ANALYZER_RF_INPUT_MAX_MHZ = 3000.0;
constexpr double LO_FREQUENCY_MIN_MHZ = 23.5;
constexpr double LO_FREQUENCY_MAX_MHZ = 6000.0;

const __FlashStringHelper* injectionLabel(LOInjectionMode mode)
{
    return (mode == LOInjectionMode::High) ? F("High") : F("Low");
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
    const MAX2871* targetLo;
    const __FlashStringHelper* label;
    double freq;
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

void handleIfmodeCommand(const char* modeToken)
{
    if (modeToken == nullptr) {
        printTechnicianBanner();
        return;
    }
    const bool highRequested = strcmp(modeToken, "high") == 0;
    const bool lowRequested  = strcmp(modeToken, "low") == 0;
    if (!highRequested && !lowRequested) {
        printTechnicianBanner();
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
        printTechnicianBanner();
        return;
    }
    const ChipTarget chipTarget = getCurrentChipTarget();
    if (chipTarget != ChipTarget::LO1 &&
        chipTarget != ChipTarget::LO2 &&
        chipTarget != ChipTarget::LO3) {
        Serial.println(F("Select lo1, lo2, or lo3 with 'chip' before using lofreq."));
        return;
    }
    char* endPointer;
    const double requestedMhz = strtod(valueToken, &endPointer);
    if (*endPointer != '\0') {
        printTechnicianBanner();
        return;
    }
    if (requestedMhz < LO_FREQUENCY_MIN_MHZ || requestedMhz > LO_FREQUENCY_MAX_MHZ) {
        Serial.println(F("LO frequency out of range (23.5 to 6000 MHz)."));
        return;
    }
    MAX2871* targetLo;
    double* reportedFreq;
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

void processSpiToken(const char* valueToken)
{
    if (valueToken == nullptr) {
        return;
    }
    ConsoleState& state = consoleState();
    if (state.chipTarget == ChipTarget::None || state.chipTarget == ChipTarget::Off) {
        printTechnicianBanner();
        return;
    }
    char* endPointer;
    const uint32_t value = static_cast<uint32_t>(strtoul(valueToken, &endPointer, 16));
    if (*endPointer != '\0') {
        printTechnicianBanner();
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
    Serial.println(F("=== Technician Console Commands ==="));
    Serial.println(F(" RFin <MHz>            0 to 3000 MHz"));
    Serial.println(F(" help                  This list"));
    Serial.println(F(" id                    Print identifier"));
    Serial.println(F(" relock                Reinitialize LO's"));
    Serial.println(F(" fulltest refcheck     Report refclock pin checks"));
    Serial.println(F(" fulltest pincheck     Report chip pin checks"));
    Serial.println(F(" fulltest atten <dB>   Program and report set point"));
    Serial.println(F(" fulltest plan <MHz>   Frequency plan (Report only)"));
    Serial.println(F(" fulltest program <lo1|lo2> Program one planned LO"));
    Serial.println(F(" ifmode <high|low>     Set injection for the selected LO"));
    Serial.println(F(" lofreq <MHz>          Set selected LO frequency"));
    Serial.println(F(" chip <target|off>     Assert target pin or set all off"));
    Serial.println(F("   targets: lo1 lo2 lo3 atten adc1 adc2 ram flash"));
    Serial.println(F(" set <ref1|ref2|off>   Enable one or disable both"));
    Serial.println(F(" spi <hex32>           Send raw 32-bit word to selected LO"));
    Serial.println(F(" <idflash|9F>          Type string or hex command value to print flash ID"));
    Serial.println(F(" <readStatReg|F|5>     Print status register configuration"));
    Serial.println();
}

void handleTechnicianCommand(const char* line, uint16_t bufferSize)
{
    // Exit, nothing to do
    if (line == nullptr) {
        return;
    }

    // Else, convert 'line' to a null-terminated string
    char buffer[bufferSize];
    strncpy(buffer, line, bufferSize);

    // Splits the command string, a.k.a. buffer contents, into individual null-terminated tokens.
    constexpr size_t NUM_TOKENS = 4U;
    char* tokens[NUM_TOKENS] = {};   // Initializes to zero's (nullptr's)
    size_t count = 0;
    char* token = strtok(buffer, " "); // Works only on the first token, index 0, in the buffer
    while (token != nullptr && count < NUM_TOKENS) {
        for (char* character = token; *character != '\0'; ++character) {
            *character = static_cast<char>(tolower(static_cast<unsigned char>(*character)));
        }
        tokens[count++] = token;    // Works on tokens at index 1 and later
        token = strtok(nullptr, " ");
    }

    if (count == 0U) {
        return;
    }

    if (tokens[1] == nullptr) { // this statement assumes that tokens[0] contains a pointer to a string
        // and it's the only token in the command line. If there is a second token, it will be ignored in this block.
        if (strcmp(tokens[0], "ascii") == 0) {
            setSerialEncoding(SerialEncoding::Ascii);
            return;
        }
        if (strcmp(tokens[0], "binary") == 0) {
            setSerialEncoding(SerialEncoding::Binary);
            return;
        }
        char* controlWordEnd;
        const uint32_t controlWord = static_cast<uint32_t>(strtoul(tokens[0], &controlWordEnd, 16));
        if (controlWordEnd != tokens[0] && *controlWordEnd == '\0') {
            processReceivedWord(controlWord);
            return;
        }
    }

    const bool rfinCommand = strcmp_P(tokens[0], PSTR("rfin")) == 0;
    if (rfinCommand && tokens[1] == nullptr) {
        printTechnicianBanner();
        return;
    }
    const char* rfinToken = rfinCommand ? tokens[1] : tokens[0];
    char* rfinEndPointer;
    const double rfinMhz = strtod(rfinToken, &rfinEndPointer);
    const bool parsedRfin = *rfinEndPointer == '\0' &&
                            (rfinCommand || tokens[1] == nullptr);
    if (parsedRfin) {
        if (rfinMhz < ANALYZER_RF_INPUT_MIN_MHZ || rfinMhz > ANALYZER_RF_INPUT_MAX_MHZ) {
            Serial.println(F("RFin out of range (0 to 3000 MHz)."));
            return;
        }
        tuneTo(rfinMhz);
        printFulltestPlanReport();
        return;
    }
    if (rfinCommand) {
        printTechnicianBanner();
        return;
    }
    if (strcmp_P(tokens[0], PSTR("help")) == 0) {
        printTechnicianBanner();
        return;
    }
    if (strcmp_P(tokens[0], PSTR("id")) == 0) {
        processReceivedWord(0x000017FFUL);
        return;
    }
    if (strcmp_P(tokens[0], PSTR("relock")) == 0) {
        processReceivedWord(0x00002FFFUL);
        return;
    }
    if (strcmp_P(tokens[0], PSTR("fulltest")) == 0) {
        handleFulltestCommand(tokens, count);
        return;
    }
    if (strcmp_P(tokens[0], PSTR("ifmode")) == 0) {
        if (count < 2U) {
            printTechnicianBanner();
            return;
        }
        handleIfmodeCommand(tokens[1]);
        return;
    }
    if (strcmp_P(tokens[0], PSTR("lofreq")) == 0) {
        if (count < 2U) {
            printTechnicianBanner();
            return;
        }
        handleLofreqCommand(tokens[1]);
        return;
    }
    if (strcmp_P(tokens[0], PSTR("chip")) == 0) {
        if (count < 2U) {
            printTechnicianBanner();
            return;
        }
        ChipTarget target = ChipTarget::None;
        if (strcmp(tokens[1], "lo1") == 0) {
            target = ChipTarget::LO1;
        }
        if (strcmp(tokens[1], "lo2") == 0) {
            target = ChipTarget::LO2;
        }
        if (strcmp(tokens[1], "lo3") == 0) {
            target = ChipTarget::LO3;
        }
        if (strcmp(tokens[1], "atten") == 0) {
            target = ChipTarget::Attenuator;
        }
        if (strcmp(tokens[1], "adc1") == 0) {
            target = ChipTarget::ADC_1;
        }
        if (strcmp(tokens[1], "adc2") == 0) {
            target = ChipTarget::ADC_2;
        }
        if (strcmp(tokens[1], "ram") == 0) {
            target = ChipTarget::RAM;
        }
        if (strcmp(tokens[1], "flash") == 0) {
            target = ChipTarget::Flash;
        }
        if (strcmp(tokens[1], "off") == 0) {
            target = ChipTarget::Off;
        }
        if (target == ChipTarget::None) {
            printTechnicianBanner();
            return;
        }
        selectSerialChipTarget(target);
        return;
    }
    if (strcmp_P(tokens[0], PSTR("set")) == 0) {
        if (count < 2U) {
            printTechnicianBanner();
            return;
        }
        ReferenceTarget target = ReferenceTarget::None;
        const __FlashStringHelper* message;
        if (strcmp(tokens[1], "ref1") == 0) {
            target = ReferenceTarget::Ref1;
            message = F("Reference clock set to REF1.");
        }
        if (strcmp(tokens[1], "ref2") == 0) {
            target = ReferenceTarget::Ref2;
            message = F("Reference clock set to REF2.");
        }
        if (strcmp(tokens[1], "off") == 0) {
            target = ReferenceTarget::Off;
            message = F("All reference clocks disabled.");
        }
        if (target == ReferenceTarget::None) {
            printTechnicianBanner();
            return;
        }
        selectRef(target);
        Serial.println(message);
        return;
    }
    if (strcmp_P(tokens[0], PSTR("spi")) == 0) {
        if (count < 2U) {
            printTechnicianBanner();
            return;
        }
        processSpiToken(tokens[1]);
        return;
    }
    if (strcmp_P(tokens[0], PSTR("idflash")) == 0) {
        processReceivedWord(0x9FU);
        return;
    }
    if (strcmp_P(tokens[0], PSTR("readstatreg")) == 0) {
        processReceivedWord(0x5U);
        return;
    }
    printTechnicianBanner();
}

// cppcheck-suppress unusedFunction
void pollTechnicianConsole()
{
    while (Serial.available() > 0) {
        const char incomingChar = static_cast<char>(Serial.read());
        const uint8_t incomingByte = static_cast<uint8_t>(incomingChar);
        // Terminate and submit technician command
        if (incomingChar == '\n') {
            lineInputBuffer[bufferIndex] = '\0';
            uint16_t bufferSize = static_cast<uint16_t>(bufferIndex + 1U);
            handleTechnicianCommand(lineInputBuffer, bufferSize);
            bufferIndex = 0U;
            continue;
        }
        // Skip non-printable characters including '\r'
        if (isprint(incomingByte) == 0) {
            continue;
        }
        // append char to command string
        if (bufferIndex < (INPUT_BUFFER_SIZE - 1U)) {
            lineInputBuffer[bufferIndex++] = incomingChar;
            continue;
        }
        bufferIndex = 0U;
        printTechnicianBanner();
    }
}
