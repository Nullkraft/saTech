#include "technician_console.h"

#include "command_interface.h"
#include "console_state.h"

#include <arduino_hal.h>
#include <ctype.h>
#include <frequency_calculator.h>
#include <math.h>
#include <max2871.h>
#include <avr/pgmspace.h>
#include <stdlib.h>
#include <string.h>

extern MAX2871 lo1;
extern MAX2871 lo2;
extern MAX2871 lo3;
extern FrequencyCalculator freqCalc;
extern double currentRfInputMhz;

void recomputePlan();
void tuneTo(double mhz);

namespace {

bool loStateForTarget(ChipTarget target, MAX2871** targetLo, double** reportedFreq);

char technicianInputBuffer[INPUT_BUFFER_SIZE];
size_t technicianInputLength = 0U;

const ChipTarget PINCHECK_TARGETS[] = {
    ChipTarget::LO1,
    ChipTarget::LO2,
    // TODO:     ChipTarget::LO3
    ChipTarget::Attenuator,
    ChipTarget::ADC_1,
    ChipTarget::ADC_2,
    ChipTarget::RAM,
    ChipTarget::Flash,
};
constexpr size_t PINCHECK_TARGET_COUNT = sizeof(PINCHECK_TARGETS) / sizeof(PINCHECK_TARGETS[0]);

constexpr double ANALYZER_RF_INPUT_MIN_MHZ = 0.0;
constexpr double ANALYZER_RF_INPUT_MAX_MHZ = 3000.0;
constexpr double LO_FREQUENCY_MIN_MHZ = 23.5;
constexpr double LO_FREQUENCY_MAX_MHZ = 6000.0;

enum class TechnicianCommandKind {
    Unknown,
    Help,
    Id,
    Relock,
    Fulltest,
    Rfin,
    Ifmode,
    Lofreq,
    Chip,
    Set,
    Spi,
    IdFlash,
};

struct TechnicianCommandMap {
    const char* token;
    TechnicianCommandKind kind;
};

const char CMD_HELP[] PROGMEM = "help";
const char CMD_ID[] PROGMEM = "id";
const char CMD_RELOCK[] PROGMEM = "relock";
const char CMD_FULLTEST[] PROGMEM = "fulltest";
const char CMD_RFIN[] PROGMEM = "rfin";
const char CMD_IFMODE[] PROGMEM = "ifmode";
const char CMD_LOFREQ[] PROGMEM = "lofreq";
const char CMD_CHIP[] PROGMEM = "chip";
const char CMD_SET[] PROGMEM = "set";
const char CMD_SPI[] PROGMEM = "spi";
const char CMD_IDFLASH[] PROGMEM = "idflash";

const TechnicianCommandMap TECHNICIAN_COMMANDS[] PROGMEM = {
    {CMD_HELP, TechnicianCommandKind::Help},
    {CMD_ID, TechnicianCommandKind::Id},
    {CMD_RELOCK, TechnicianCommandKind::Relock},
    {CMD_FULLTEST, TechnicianCommandKind::Fulltest},
    {CMD_RFIN, TechnicianCommandKind::Rfin},
    {CMD_IFMODE, TechnicianCommandKind::Ifmode},
    {CMD_LOFREQ, TechnicianCommandKind::Lofreq},
    {CMD_CHIP, TechnicianCommandKind::Chip},
    {CMD_SET, TechnicianCommandKind::Set},
    {CMD_SPI, TechnicianCommandKind::Spi},
    {CMD_IDFLASH, TechnicianCommandKind::IdFlash},
};
constexpr size_t TECHNICIAN_COMMAND_COUNT = sizeof(TECHNICIAN_COMMANDS) / sizeof(TECHNICIAN_COMMANDS[0]);

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

void lowercaseInPlace(char* text)
{
    if (text == nullptr) {
        return;
    }
    while (*text != '\0') {
        *text = static_cast<char>(tolower(static_cast<unsigned char>(*text)));
        ++text;
    }
}

const __FlashStringHelper* injectionLabel(LOInjectionMode mode)
{
    return (mode == LOInjectionMode::High) ? F("High") : F("Low");
}

const ChipSelectDefinition* pincheckDefinitionForTarget(ChipTarget target)
{
    for (size_t i = 0; i < CHIP_COUNT; ++i) {
        if (CHIP_DEFINITIONS[i].chip == target) {
            return &CHIP_DEFINITIONS[i];
        }
    }
    return nullptr;
}

void printJsonReportEvent(const __FlashStringHelper* event, const __FlashStringHelper* report)
{
    Serial.print(F("{\"type\":\""));
    Serial.print(event);
    Serial.print(F("\",\"report\":\""));
    Serial.print(report);
    Serial.println(F("\"}"));
}

void printJsonValue(const __FlashStringHelper* name, double value, uint8_t digits)
{
    Serial.print(F("{\"type\":\"value\",\"name\":\""));
    Serial.print(name);
    Serial.print(F("\",\"value\":"));
    Serial.print(value, digits);
    Serial.println(F("}"));
}

void printJsonTextValue(const __FlashStringHelper* name, const __FlashStringHelper* value)
{
    Serial.print(F("{\"type\":\"value\",\"name\":\""));
    Serial.print(name);
    Serial.print(F("\",\"value\":\""));
    Serial.print(value);
    Serial.println(F("\"}"));
}

void printReferenceState(const __FlashStringHelper* prefix, uint8_t ref1, uint8_t ref2)
{
    Serial.print(prefix);
    Serial.print(ref1 == HIGH ? F("on") : F("off"));
    Serial.print(F(" : Ref2 "));
    Serial.print(ref2 == HIGH ? F("on") : F("off"));
}

void printJsonReferenceCheck(const __FlashStringHelper* name, uint8_t expectedRef1, uint8_t expectedRef2)
{
    const uint8_t actualRef1 = digitalRead(PIN_REF_EN1);
    const uint8_t actualRef2 = digitalRead(PIN_REF_EN2);
    Serial.print(F("{\"type\":\"check\",\"name\":\""));
    Serial.print(name);
    Serial.print(F("\",\"expected\":\""));
    printReferenceState(F("Ref1 "), expectedRef1, expectedRef2);
    Serial.print(F("\",\"actual\":\""));
    printReferenceState(F("Ref1 "), actualRef1, actualRef2);
    Serial.print(F("\",\"result\":\""));
    Serial.print((actualRef1 == expectedRef1 && actualRef2 == expectedRef2) ? F("PASS") : F("FAIL"));
    Serial.println(F("\"}"));
}

bool chipStateMatches(ChipTarget selected)
{
    for (size_t i = 0; i < PINCHECK_TARGET_COUNT; ++i) {
        const ChipTarget target = PINCHECK_TARGETS[i];
        const ChipSelectDefinition* def = pincheckDefinitionForTarget(target);
        if (def == nullptr) {
            return false;
        }
        const bool actualAsserted = digitalRead(def->pin) == def->assertedLevel;
        const bool expectedAsserted = target == selected;
        if (actualAsserted != expectedAsserted) {
            return false;
        }
    }
    return true;
}

void writeChipSelectState(ChipTarget selected)
{
    for (size_t i = 0; i < CHIP_COUNT; ++i) {
        const ChipSelectDefinition& def = CHIP_DEFINITIONS[i];
        digitalWrite(def.pin, (def.assertedLevel == HIGH) ? LOW : HIGH);
    }
    const ChipSelectDefinition* selectedDef = pincheckDefinitionForTarget(selected);
    if (selectedDef != nullptr) {
        digitalWrite(selectedDef->pin, selectedDef->assertedLevel);
    }
}

void printPincheckName(size_t index)
{
    switch (index) {
        case 0: Serial.print(F("lo1")); break;
        case 1: Serial.print(F("lo2")); break;
        case 2: Serial.print(F("atten")); break;
        case 3: Serial.print(F("adc1")); break;
        case 4: Serial.print(F("adc2")); break;
        case 5: Serial.print(F("ram")); break;
        case 6: Serial.print(F("flash")); break;
        case 7: Serial.print(F("chips_off")); break;
        default: break;
    }
}

void printPincheckFailures(const bool results[])
{
    bool first = true;
    Serial.print(F("failed: "));
    for (size_t i = 0; i < 8U; ++i) {
        if (results[i]) {
            continue;
        }
        if (!first) {
            Serial.print(F(", "));
        }
        printPincheckName(i);
        first = false;
    }
}

long roundMilliMhz(double value)
{
    return static_cast<long>(round(value * 1000.0));
}

void printLoFrequencyCheck(const __FlashStringHelper* prefix, double expectedMhz, double actualMhz)
{
    const long expectedMilliMhz = roundMilliMhz(expectedMhz);
    const long actualMilliMhz = roundMilliMhz(actualMhz);
    const double expectedRoundedMhz = static_cast<double>(expectedMilliMhz) / 1000.0;
    const double actualRoundedMhz = static_cast<double>(actualMilliMhz) / 1000.0;
    Serial.print(F("{\"type\":\"value\",\"name\":\""));
    Serial.print(prefix);
    Serial.print(F("_frequency_mhz\",\"value\":"));
    Serial.print(actualRoundedMhz, 3);
    Serial.println(F("}"));
    Serial.print(F("{\"type\":\"check\",\"name\":\""));
    Serial.print(prefix);
    Serial.print(F("_frequency_mhz\",\"expected\":"));
    Serial.print(expectedRoundedMhz, 3);
    Serial.print(F(",\"actual\":"));
    Serial.print(actualRoundedMhz, 3);
    Serial.print(F(",\"result\":\""));
    Serial.print(actualMilliMhz == expectedMilliMhz ? F("PASS") : F("FAIL"));
    Serial.println(F("\"}"));
}

void printLoRegisterValues(const __FlashStringHelper* prefix, const MAX2871& lo)
{
    Serial.print(F("{\"type\":\"value\",\"name\":\""));
    Serial.print(prefix);
    Serial.print(F("_m\",\"value\":"));
    Serial.print(lo.M);
    Serial.println(F("}"));
    Serial.print(F("{\"type\":\"value\",\"name\":\""));
    Serial.print(prefix);
    Serial.print(F("_f\",\"value\":"));
    Serial.print(lo.Frac);
    Serial.println(F("}"));
    Serial.print(F("{\"type\":\"value\",\"name\":\""));
    Serial.print(prefix);
    Serial.print(F("_n\",\"value\":"));
    Serial.print(lo.N);
    Serial.println(F("}"));
    Serial.print(F("{\"type\":\"value\",\"name\":\""));
    Serial.print(prefix);
    Serial.print(F("_diva\",\"value\":"));
    Serial.print(1 << lo.DIVA);
    Serial.println(F("}"));
}

void printJsonError(const __FlashStringHelper* command,
                    const __FlashStringHelper* code,
                    const __FlashStringHelper* message)
{
    Serial.print(F("{\"type\":\"error\",\"command\":\""));
    Serial.print(command);
    Serial.print(F("\",\"code\":\""));
    Serial.print(code);
    Serial.print(F("\",\"message\":\""));
    Serial.print(message);
    Serial.println(F("\"}"));
}

void stageFulltestPlan(double rfinMhz)
{
    ConsoleState& state = consoleState();
    currentRfInputMhz = rfinMhz;
    state.lo1Manual = false;
    state.lo2Manual = false;
    state.lo3Manual = false;
    freqCalc.compute_LO_frequencies(currentRfInputMhz, freqCalc.RefClock1, 1,
                                    state.desiredLo2Injection,
                                    state.desiredLo3Injection);
    lo1.freq2FMN(freqCalc.FreqLO1);
    lo2.freq2FMN(freqCalc.FreqLO2);
    lo3.freq2FMN(freqCalc.FreqLO3);
}

void handleFulltestPlan(const char* valueToken)
{
    if (valueToken == nullptr) {
        printJsonError(F("fulltest plan"), F("missing_argument"), F("RFin MHz is required"));
        return;
    }
    char* endPointer;
    const double rfinMhz = strtod(valueToken, &endPointer);
    if (endPointer == valueToken || *endPointer != '\0') {
        printJsonError(F("fulltest plan"), F("invalid_argument"), F("RFin MHz must be numeric"));
        return;
    }
    if (rfinMhz < ANALYZER_RF_INPUT_MIN_MHZ || rfinMhz > ANALYZER_RF_INPUT_MAX_MHZ) {
        printJsonError(F("fulltest plan"), F("invalid_rfin"), F("RFin out of range"));
        return;
    }

    stageFulltestPlan(rfinMhz);
    printFulltestPlanReport();
}

void handleFulltestAtten(const char* valueToken)
{
    if (valueToken == nullptr) {
        printJsonError(F("fulltest atten"), F("missing_argument"), F("Attenuator dB is required"));
        return;
    }
    char* endPointer;
    const double requestedDb = strtod(valueToken, &endPointer);
    if (endPointer == valueToken || *endPointer != '\0') {
        printJsonError(F("fulltest atten"), F("invalid_argument"), F("Attenuator dB must be numeric"));
        return;
    }
    if (requestedDb < ATTEN_MIN_DB || requestedDb > ATTEN_MAX_DB) {
        printJsonError(F("fulltest atten"), F("invalid_argument"), F("Attenuator out of range"));
        return;
    }
    const double steps = (requestedDb - ATTEN_MIN_DB) / ATTEN_STEP_DB;
    if (fabs(steps - round(steps)) > 0.01) {
        printJsonError(F("fulltest atten"), F("invalid_argument"), F("Attenuator step is 0.25 dB"));
        return;
    }

    programAttenuatorDb(requestedDb);
    printJsonReportEvent(F("report_begin"), F("atten"));
    printJsonValue(F("atten_db"), getCurrentAttenuatorDb(), 2);
    printJsonReportEvent(F("report_end"), F("atten"));
}

void handleFulltestProgram(const char* loToken)
{
    if (loToken == nullptr) {
        printJsonError(F("fulltest program"), F("missing_argument"), F("Expected lo1 or lo2"));
        return;
    }

    ChipTarget target = ChipTarget::None;
    const __FlashStringHelper* prefix = nullptr;
    const __FlashStringHelper* report = nullptr;
    double plannedFrequency = 0.0;
    if (strcmp(loToken, "lo1") == 0) {
        target = ChipTarget::LO1;
        prefix = F("lo1");
        report = F("program_lo1");
        plannedFrequency = freqCalc.FreqLO1;
    } else if (strcmp(loToken, "lo2") == 0) {
        target = ChipTarget::LO2;
        prefix = F("lo2");
        report = F("program_lo2");
        plannedFrequency = freqCalc.FreqLO2;
    } else {
        printJsonError(F("fulltest program"), F("invalid_lo"), F("Expected lo1 or lo2"));
        return;
    }

    if (freqCalc.FreqRFin == 0.0 || plannedFrequency == 0.0) {
        printJsonError(F("fulltest program"), F("no_frequency_plan"), F("Run fulltest plan before programming LO"));
        return;
    }

    MAX2871* targetLo = nullptr;
    double* reportedFreq = nullptr;
    if (!loStateForTarget(target, &targetLo, &reportedFreq)) {
        printJsonError(F("fulltest program"), F("invalid_lo"), F("Expected lo1 or lo2"));
        return;
    }

    targetLo->setFrequency(plannedFrequency);
    *reportedFreq = targetLo->fmn2freq();
    printJsonReportEvent(F("report_begin"), report);
    printLoFrequencyCheck(prefix, plannedFrequency, *reportedFreq);
    printLoRegisterValues(prefix, *targetLo);
    printJsonReportEvent(F("report_end"), report);
}

void handleFulltestPincheck()
{
    bool results[8];
    printJsonReportEvent(F("report_begin"), F("pincheck"));
    writeChipSelectState(ChipTarget::LO1);
    results[0] = chipStateMatches(ChipTarget::LO1);
    writeChipSelectState(ChipTarget::LO2);
    results[1] = chipStateMatches(ChipTarget::LO2);
    writeChipSelectState(ChipTarget::Attenuator);
    results[2] = chipStateMatches(ChipTarget::Attenuator);
    writeChipSelectState(ChipTarget::ADC_1);
    results[3] = chipStateMatches(ChipTarget::ADC_1);
    writeChipSelectState(ChipTarget::ADC_2);
    results[4] = chipStateMatches(ChipTarget::ADC_2);
    writeChipSelectState(ChipTarget::RAM);
    results[5] = chipStateMatches(ChipTarget::RAM);
    writeChipSelectState(ChipTarget::Flash);
    results[6] = chipStateMatches(ChipTarget::Flash);
    writeChipSelectState(ChipTarget::None);
    results[7] = chipStateMatches(ChipTarget::None);
    const bool passed = results[0] && results[1] && results[2] && results[3] &&
                        results[4] && results[5] && results[6] && results[7];
    Serial.print(F("{\"type\":\"check\",\"name\":\"pin_checks\",\"expected\":\"PASS\",\"actual\":\""));
    if (passed) {
        Serial.print(F("PASS"));
    } else {
        printPincheckFailures(results);
    }
    Serial.print(F("\",\"result\":\""));
    Serial.print(passed ? F("PASS") : F("FAIL"));
    Serial.println(F("\"}"));
    printJsonReportEvent(F("report_end"), F("pincheck"));
}

void handleFulltestCommand(char* const tokens[], size_t count)
{
    if (count < 2U) {
        printJsonError(F("fulltest"), F("missing_argument"), F("Expected refcheck, pincheck, plan, or program"));
        return;
    }
    if (strcmp(tokens[1], "plan") == 0) {
        handleFulltestPlan(count >= 3U ? tokens[2] : nullptr);
        return;
    }
    if (strcmp(tokens[1], "atten") == 0) {
        handleFulltestAtten(count >= 3U ? tokens[2] : nullptr);
        return;
    }
    if (strcmp(tokens[1], "program") == 0) {
        handleFulltestProgram(count >= 3U ? tokens[2] : nullptr);
        return;
    }
    if (strcmp(tokens[1], "pincheck") == 0) {
        handleFulltestPincheck();
        return;
    }
    if (strcmp(tokens[1], "refcheck") != 0) {
        printJsonError(F("fulltest"), F("unsupported_command"), F("Expected refcheck, pincheck, plan, or program"));
        return;
    }

    printJsonReportEvent(F("report_begin"), F("refcheck"));
    selectRef(ReferenceTarget::Ref1);
    printJsonReferenceCheck(F("ref1_selected"), HIGH, LOW);
    selectRef(ReferenceTarget::Ref2);
    printJsonReferenceCheck(F("ref2_selected"), LOW, HIGH);
    selectRef(ReferenceTarget::None);
    printJsonReferenceCheck(F("refs_off"), LOW, LOW);
    printJsonReportEvent(F("report_end"), F("refcheck"));
}

bool parseControlWord(const char* token, uint32_t* word)
{
    char* endPointer;
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
    if (strcmp(targetToken, "lo1") == 0) {
        *selector = 0x01FFU;
        return true;
    } else if (strcmp(targetToken, "lo2") == 0) {
        *selector = 0x02FFU;
        return true;
    } else if (strcmp(targetToken, "lo3") == 0) {
        *selector = 0x03FFU;
        return true;
    } else if (strcmp(targetToken, "adc1") == 0) {
        *selector = 0x05FFU;
        return true;
    } else if (strcmp(targetToken, "adc2") == 0) {
        *selector = 0x0DFFU;
        return true;
    } else if (strcmp(targetToken, "ram") == 0) {
        *selector = 0x15FFU;
        return true;
    } else if (strcmp(targetToken, "flash") == 0) {
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
    if (strcmp(targetToken, "atten") == 0) {
        selectSerialChipTarget(ChipTarget::Attenuator);
        return;
    }
    if (strcmp(targetToken, "off") == 0) {
        selectSerialChipTarget(ChipTarget::None);
        return;
    }
    printTechnicianBanner();
}

bool referenceSelectorForToken(const char* targetToken, uint16_t* selector)
{
    if (targetToken == nullptr || selector == nullptr) {
        return false;
    }
    if (strcmp(targetToken, "ref1") == 0) {
        *selector = 0x0CFFU;
        return true;
    } else if (strcmp(targetToken, "ref2") == 0) {
        *selector = 0x14FFU;
        return true;
    } else if (strcmp(targetToken, "off") == 0) {
        *selector = 0x04FFU;
        return true;
    }
    return false;
}

void processSetToken(const char* targetToken)
{
    if (targetToken == nullptr) {
        printTechnicianBanner();
        return;
    }
    uint16_t selector = 0U;
    if (!referenceSelectorForToken(targetToken, &selector)) {
        printTechnicianBanner();
        return;
    }
    processReceivedWord(static_cast<uint32_t>(selector));
    if (selector == 0x0CFFU) {
        Serial.println(F("Reference clock set to REF1."));
    } else if (selector == 0x14FFU) {
        Serial.println(F("Reference clock set to REF2."));
    } else {
        Serial.println(F("All reference clocks disabled."));
    }
}

void processSpiToken(const char* valueToken)
{
    if (valueToken == nullptr) {
        return;
    }
    ConsoleState& state = consoleState();
    if (state.chipTarget == ChipTarget::None) {
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

TechnicianCommandKind commandKindFromToken(const char* token)
{
    for (size_t i = 0; i < TECHNICIAN_COMMAND_COUNT; ++i) {
        const char* commandToken = reinterpret_cast<const char*>(
            pgm_read_word(&TECHNICIAN_COMMANDS[i].token));
        if (strcmp_P(token, commandToken) == 0) {
            return static_cast<TechnicianCommandKind>(
                pgm_read_byte(&TECHNICIAN_COMMANDS[i].kind));
        }
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

void printFulltestPlanReport()
{
    const ConsoleState& state = consoleState();
    printJsonReportEvent(F("report_begin"), F("plan"));
    printJsonValue(F("rfin_mhz"), freqCalc.FreqRFin, 3);
    printJsonValue(F("if1_mhz"), freqCalc.IF1, 3);
    printJsonValue(F("if2_mhz"), freqCalc.IF2, 3);
    printLoFrequencyCheck(F("lo1"), freqCalc.FreqLO1, freqCalc.FreqLO1);
    printLoRegisterValues(F("lo1"), lo1);
    printLoFrequencyCheck(F("lo2"), freqCalc.FreqLO2, freqCalc.FreqLO2);
    printLoRegisterValues(F("lo2"), lo2);
    printJsonTextValue(F("lo1_injection"), state.lo1Manual ? F("Manual") : injectionLabel(freqCalc.LO1InjectionMode));
    printJsonTextValue(F("lo2_injection"), state.lo2Manual ? F("Manual") : injectionLabel(freqCalc.LO2InjectionMode));
    printJsonTextValue(F("lo3_injection"), state.lo3Manual ? F("Manual") : injectionLabel(freqCalc.LO3InjectionMode));
    printJsonValue(F("atten_db"), getCurrentAttenuatorDb(), 2);
    printJsonTextValue(F("chip_select"), chipTargetName(getCurrentChipTarget()));
    printJsonReportEvent(F("report_end"), F("plan"));
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
    Serial.println(F(" idflash <hex32>       Send raw 32-bit word to rint flash ID"));
    Serial.println();
}

void handleTechnicianCommand(const char* line)
{
    // Exit, nothing to do
    if (line == nullptr) {
        return;
    }

    // Else, convert 'line' to a null-terminated string
    char buffer[INPUT_BUFFER_SIZE];
    strncpy(buffer, line, sizeof(buffer) - 1U);
    buffer[sizeof(buffer) - 1U] = '\0';
    trimWhitespace(buffer);

    // Exit if technician just pressed "enter" ('buffer' is an empty string)
    if (buffer[0] == '\0') {
        return;
    }

    // Split command string into individual words and store in tokens array
    char* tokens[4] = {nullptr, nullptr, nullptr, nullptr};
    size_t count = 0;
    char* token = strtok(buffer, " ");
    while (token != nullptr && count < (sizeof(tokens) / sizeof(tokens[0]))) {
        lowercaseInPlace(token);
        tokens[count++] = token;
        token = strtok(nullptr, " ");
    }

    if (tokens[1] == nullptr) {
        if (strcmp(tokens[0], "ascii") == 0) {
            setSerialEncoding(SerialEncoding::Ascii);
            return;
        }
        if (strcmp(tokens[0], "binary") == 0) {
            setSerialEncoding(SerialEncoding::Binary);
            return;
        }
        uint32_t controlWord = 0U;
        if (parseControlWord(tokens[0], &controlWord)) {
            processReceivedWord(controlWord);
            return;
        }
    }

    char* endPointer;
    const double mhz = strtod(tokens[0], &endPointer);
    const bool parsedNumber = (*endPointer == '\0') && (tokens[1] == nullptr);
    if (parsedNumber) {
        if (mhz < ANALYZER_RF_INPUT_MIN_MHZ || mhz > ANALYZER_RF_INPUT_MAX_MHZ) {
            Serial.println(F("RFin out of range (0 to 3000 MHz)."));
            return;
        }
        tuneTo(mhz);
        printFulltestPlanReport();
        return;
    }

    switch (commandKindFromToken(tokens[0])) {
        case TechnicianCommandKind::Help:
            printTechnicianBanner();
            return;
        case TechnicianCommandKind::Id:
            processReceivedWord(0x000017FFUL);
            return;
        case TechnicianCommandKind::Relock:
            processReceivedWord(0x00002FFFUL);
            return;
        case TechnicianCommandKind::Fulltest:
            handleFulltestCommand(tokens, count);
            return;
        case TechnicianCommandKind::Rfin: {
            if (count < 2U) {
                printTechnicianBanner();
                return;
            }
            char* rfinEndPointer;
            const double rfinMhz = strtod(tokens[1], &rfinEndPointer);
            if (rfinEndPointer == tokens[1] || *rfinEndPointer != '\0') {
                printTechnicianBanner();
                return;
            }
            if (rfinMhz < ANALYZER_RF_INPUT_MIN_MHZ || rfinMhz > ANALYZER_RF_INPUT_MAX_MHZ) {
                Serial.println(F("RFin out of range (0 to 3000 MHz)."));
                return;
            }
            tuneTo(rfinMhz);
            printFulltestPlanReport();
            return;
        }
        case TechnicianCommandKind::Ifmode:
            if (count < 2U) {
                printTechnicianBanner();
                return;
            }
            handleIfmodeCommand(tokens[1]);
            return;
        case TechnicianCommandKind::Lofreq:
            if (count < 2U) {
                printTechnicianBanner();
                return;
            }
            handleLofreqCommand(tokens[1]);
            return;
        case TechnicianCommandKind::Chip:
            if (count < 2U) {
                printTechnicianBanner();
                return;
            }
            processChipToken(tokens[1]);
            return;
        case TechnicianCommandKind::Set:
            if (count < 2U) {
                printTechnicianBanner();
                return;
            }
            processSetToken(tokens[1]);
            return;
        case TechnicianCommandKind::Spi:
            if (count < 2U) {
                printTechnicianBanner();
                return;
            }
            processSpiToken(tokens[1]);
            return;
        case TechnicianCommandKind::IdFlash:
            processReceivedWord(0x9FU);
            return;
        case TechnicianCommandKind::Unknown:
        default:
            printTechnicianBanner();
            return;
    }
}

// cppcheck-suppress unusedFunction
void pollTechnicianConsole()
{
    while (Serial.available() > 0) {
        const char incomingChar = static_cast<char>(Serial.read());
        const uint8_t incomingByte = static_cast<uint8_t>(incomingChar);
        // Terminate and submit technician command
        if (incomingChar == '\n') {
            technicianInputBuffer[technicianInputLength] = '\0';
            handleTechnicianCommand(technicianInputBuffer);
            technicianInputLength = 0U;
            continue;
        }
        // Skip non-printable characters including '\r'
        if (isprint(incomingByte) == 0) {
            continue;
        }
        // append char to command string
        if (technicianInputLength < (INPUT_BUFFER_SIZE - 1U)) {
            technicianInputBuffer[technicianInputLength++] = incomingChar;
            continue;
        }
        technicianInputLength = 0U;
        Serial.println(F("Input too long, line cleared."));
    }
}
