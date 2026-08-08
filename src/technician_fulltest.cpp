#include "technician_fulltest.h"

#include "board_devices.h"
#include "command_interface.h"
#include "console_state.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace {

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
    const uint32_t packedFMN = (static_cast<uint32_t>(lo.Frac) << 20) |
                               (static_cast<uint32_t>(lo.M) << 8) |
                               lo.N;
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
    Serial.print(F("_packed_fmn\",\"value\":"));
    Serial.print(packedFMN);
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
    state.lo1Manual = false;        // set to automatic frequency control for LO1
    state.lo2Manual = false;        // set to automatic frequency control for LO2
    state.lo3Manual = false;        // set to automatic frequency control for LO3
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

    MAX2871* targetLo;
    const __FlashStringHelper* prefix;
    const __FlashStringHelper* report;
    double* reportedFreq;
    double plannedFrequency;
    if (strcmp(loToken, "lo1") == 0) {
        targetLo = &lo1;
        prefix = F("lo1");
        report = F("program_lo1");
        reportedFreq = &freqCalc.FreqLO1;
        plannedFrequency = freqCalc.FreqLO1;
    } else if (strcmp(loToken, "lo2") == 0) {
        targetLo = &lo2;
        prefix = F("lo2");
        report = F("program_lo2");
        reportedFreq = &freqCalc.FreqLO2;
        plannedFrequency = freqCalc.FreqLO2;
    } else {
        printJsonError(F("fulltest program"), F("invalid_lo"), F("Expected lo1 or lo2"));
        return;
    }

    if (freqCalc.FreqRFin == 0.0 || plannedFrequency == 0.0) {
        printJsonError(F("fulltest program"), F("no_frequency_plan"), F("Run fulltest plan before programming LO"));
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
    writeChipSelectState(ChipTarget::Off);
    results[7] = chipStateMatches(ChipTarget::Off);
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

} // namespace

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
    selectRef(ReferenceTarget::Off);
    printJsonReferenceCheck(F("refs_off"), LOW, LOW);
    printJsonReportEvent(F("report_end"), F("refcheck"));
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
