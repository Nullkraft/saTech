#include "technician_console.h"

#include "board_devices.h"
#include "command_Codes.h"
#include "command_interface.h"
#include "console_state.h"
#include "technician_fulltest.h"

#include <ctype.h>
#include <avr/pgmspace.h>
#include <stdlib.h>
#include <string.h>

namespace {

char lineInputBuffer[INPUT_BUFFER_SIZE];
size_t bufferIndex = 0U;

constexpr double ANALYZER_RF_INPUT_MIN_MHZ = 0.0;
constexpr double ANALYZER_RF_INPUT_MAX_MHZ = 3000.0;
constexpr double LO_FREQUENCY_MIN_MHZ = 23.5;
constexpr double LO_FREQUENCY_MAX_MHZ = 6000.0;

constexpr uint32_t technicianInstructionWord(uint8_t commandBits, uint8_t chipAddress)
{
    return (static_cast<uint32_t>(commandBits) << 12) | (static_cast<uint32_t>(chipAddress) << 8) | InstructionCommandFlag;
    // One packed 32 bit value : cmd[15:12]+addr[11:8]+flag[7:0]
}

const __FlashStringHelper* injectionLabel(LOInjectionMode mode)
{
    return (mode == LOInjectionMode::High) ? F("High") : F("Low");
}

void logManualWrite(uint32_t value)
{
    Serial.print(F("[SPI] "));
    Serial.print(chipTargetName(getCurrentChipTarget()));
    Serial.print(F("=0x"));
    Serial.println(value, HEX);
}

void printSelectedLoSnapshot(ChipTarget target)
{
    const MAX2871* targetLo;
    const __FlashStringHelper* label;
    uint32_t freq;
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
    Serial.print(freq / 1000.0, 3);
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
    const bool highRequested = strcmp(modeToken, "high") == 0;
    const bool lowRequested  = strcmp(modeToken, "low") == 0;
    if (!highRequested && !lowRequested) {
        printTechnicianBanner();
        return;
    }
    ConsoleState& state = consoleState();
    if (state.chipTarget == ChipTarget::LO1) {
        Serial.println(F("LO1 ifmode is automatic."));
        return;
    }
    if (state.chipTarget != ChipTarget::LO2 && state.chipTarget != ChipTarget::LO3) {
        Serial.println(F("Select lo2 or lo3 first."));
        return;
    }
    if ((state.chipTarget == ChipTarget::LO2 && state.lo2Manual) ||
        (state.chipTarget == ChipTarget::LO3 && state.lo3Manual)) {
        Serial.println(F("Manual lofreq active; use RFin to restore auto."));
        return;
    }
    const LOInjectionMode requestedMode = highRequested ? LOInjectionMode::High : LOInjectionMode::Low;
    if (state.chipTarget == ChipTarget::LO2) {
        state.desiredLo2Injection = requestedMode;
    } else {
        // state.chipTarget is ChipTarget::LO3 after the target check above.
        state.desiredLo3Injection = requestedMode;
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
    const ChipTarget chipTarget = getCurrentChipTarget();
    if (chipTarget != ChipTarget::LO1 &&
        chipTarget != ChipTarget::LO2 &&
        chipTarget != ChipTarget::LO3) {
        Serial.println(F("Select lo1, lo2, or lo3 first."));
        return;
    }
    char* endPointer;
    const double requestedMhz = strtod(valueToken, &endPointer);
    if (*endPointer != '\0') {
        printTechnicianBanner();
        return;
    }
    if (requestedMhz < LO_FREQUENCY_MIN_MHZ || requestedMhz > LO_FREQUENCY_MAX_MHZ) {
        Serial.println(F("lofreq out of range."));
        return;
    }
    uint32_t actual;
    if (chipTarget == ChipTarget::LO1) {
        lo1.setFrequency(static_cast<uint32_t>(round(requestedMhz * 1000.0)));
        actual = lo1.fmn2freq();
        freqCalc.FreqLO1 = actual;
        consoleState().lo1Manual = true;
    } else if (chipTarget == ChipTarget::LO2) {
        lo2.setFrequency(static_cast<uint32_t>(round(requestedMhz * 1000.0)));
        actual = lo2.fmn2freq();
        freqCalc.FreqLO2 = actual;
        consoleState().lo2Manual = true;
    } else {
        lo3.setFrequency(static_cast<uint32_t>(round(requestedMhz * 1000.0)));
        actual = lo3.fmn2freq();
        freqCalc.FreqLO3 = actual;
        consoleState().lo3Manual = true;
    }

    printSelectedLoSnapshot(chipTarget);
    printInjectionSummary();
    Serial.println();
}

void processSpiToken(const char* valueToken)
{
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
            Serial.println(F("Repeat same SPI command to arm."));
            return;
        }
        if (state.pendingSpiValue != value || state.pendingSpiTarget != state.chipTarget) {
            state.pendingSpiValue  = value;
            state.pendingSpiTarget = state.chipTarget;
            Serial.println(F("SPI mismatch; repeat desired command."));
            return;
        }
        state.pendingSpiConfirmation = false;
        state.manualSpiArmed         = true;
        Serial.println(F("Manual SPI writes armed."));
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
    Serial.println(F(" RFin <MHz>            0-3000 MHz"));
    Serial.println(F(" help"));
    Serial.println(F(" id                    Print identifier"));
    Serial.println(F(" report                Print frequency plan"));
    Serial.println(F(" relock                Reinitialize LO's"));
    Serial.println(F(" fulltest: refcheck pincheck atten <dB>"));
    Serial.println(F("           plan <MHz> program <lo1|lo2>"));
    Serial.println(F(" ifmode <high|low>     Set LO injection mode"));
    Serial.println(F(" lofreq <MHz>          Set target LO frequency"));
    Serial.println(F(" chip <target|off>     Assert chip pin or all off"));
    Serial.println(F("   targets: lo1 lo2 lo3 atten adc1 adc2 ram flash"));
    Serial.println(F(" set <ref1|ref2|off>   Enable one or both off"));
    Serial.println(F(" spi <hex32>           Send raw 32-bit word to selected LO"));
    Serial.println(F(" idflash               Print the flash ID code"));
    Serial.println(F(" readStatReg           Print status register config"));
    Serial.println();
}

void handleTechnicianCommand(const char* line, uint16_t bufferSize)
{
    // Convert 'line' to a null-terminated string
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
        tuneTo(static_cast<uint32_t>(round(rfinMhz * 1000.0)));
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
        processReceivedWord(technicianInstructionWord(CmdMessageRequest, AddrMessages));
        return;
    }
    if (strcmp_P(tokens[0], PSTR("report")) == 0) {
        printFulltestPlanReport();
        return;
    }
    if (strcmp_P(tokens[0], PSTR("relock")) == 0) {
        processReceivedWord(technicianInstructionWord(CmdResetHardwareReportPllStatus, AddrMessages));
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
        selectChip(target);
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
        processReceivedWord(technicianInstructionWord(CmdFlashId, AddrFlash));
        return;
    }
    if (strcmp_P(tokens[0], PSTR("readstatreg")) == 0) {
        processReceivedWord(technicianInstructionWord(CmdFlashRegisterReport, AddrFlash));
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
