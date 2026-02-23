#if defined(PIO_UNIT_TESTING)

#elif defined(ARDUINO)
#include <Arduino.h>
#include <SPI.h>
#include <arduino_hal.h>
#include <frequency_calculator.h>
#include <max2871.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Metro Mini pinout (see MAX2871 examples/specAnn/specAnn.ino)
static constexpr uint8_t PIN_ATTEN   = A5;
static constexpr uint8_t PIN_LE_LO1  = A3;
static constexpr uint8_t PIN_LE_LO2  = 4;
static constexpr uint8_t PIN_LE_LO3  = A4;
static constexpr uint8_t PIN_REF_EN1 = 5;
static constexpr uint8_t PIN_REF_EN2 = 6;
static constexpr uint8_t PIN_STATUS  = 10;
static constexpr double  REF_MHZ     = 66.0;
static constexpr double  STARTUP_RF_MHZ = 1735.113;
static constexpr double  MIN_RF_INPUT_MHZ = 23.5;
static constexpr double  MAX_RF_INPUT_MHZ = 6000.0;
static constexpr size_t  INPUT_BUFFER_SIZE = 96;
static constexpr uint16_t HEARTBEAT_INTERVAL_MS = 1;
static constexpr double ATTEN_MIN_DB = 1.0;
static constexpr double ATTEN_MAX_DB = 31.75;
static constexpr double ATTEN_STEP_DB = 0.25;
static constexpr uint32_t ATTEN_SPI_HZ = 1000000UL;

static ArduinoHAL halLo1(PIN_LE_LO1);
static ArduinoHAL halLo2(PIN_LE_LO2);
static ArduinoHAL halLo3(PIN_LE_LO3);

static MAX2871 lo1(REF_MHZ, halLo1);
static MAX2871 lo2(REF_MHZ, halLo2);
static MAX2871 lo3(REF_MHZ, halLo3);

static FrequencyCalculator freqCalc(lo1, lo2, lo3);

enum class ChipTarget { None, LO1, LO2, LO3, Attenuator, Aux };

static char inputBuffer[INPUT_BUFFER_SIZE];
static size_t inputLength = 0;
static double currentRfInputMhz = STARTUP_RF_MHZ;
static double currentAttenuatorDb = ATTEN_MIN_DB;
static ChipTarget currentChipTarget = ChipTarget::None;
static bool manualSpiArmed = false;
static bool pendingSpiConfirmation = false;
static uint32_t pendingSpiValue = 0;
static ChipTarget pendingSpiTarget = ChipTarget::None;
static uint32_t lastHeartbeatToggleMs = 0;
static bool heartbeatState = false;
static LOInjectionMode desiredLo1Injection = LOInjectionMode::High;
static LOInjectionMode desiredLo2Injection = LOInjectionMode::High;
static LOInjectionMode desiredLo3Injection = LOInjectionMode::High;

static void printBanner();
static void initializeLo(MAX2871& lo)
{
#if !defined(SPECANN_CI_BUILD)
    lo.begin();
    lo.outputSelect(RF_ALL);
    lo.outputPower(+5, RF_ALL);
#else
    (void)lo;
#endif
}
static void printFrequencyPlan();
static void printLoSummary(const __FlashStringHelper* label, const MAX2871& lo);
static void printStatus();
static void tuneTo(double mhz);
static void recomputePlan();
static void pollSerial();
static void handleCommand(const char* line);
static void trimWhitespace(char* text);
static void heartbeat();
static bool equalsIgnoreCase(const char* lhs, const char* rhs);
static void handleAttenuatorCommand(const char* valueToken);
static void handleIfmodeCommand(const char* loToken, const char* modeToken);
static void handleChipCommand(const char* targetToken);
static void handleSpiCommand(const char* valueToken);
static void selectChip(ChipTarget target);
static void deassertTarget(ChipTarget target);
static const __FlashStringHelper* chipTargetName(ChipTarget target);
static const __FlashStringHelper* injectionModeName(LOInjectionMode mode);
static void programAttenuatorDb(double db);
static uint8_t attenCodeFromDb(double db);
static void programAttenuatorRaw(uint8_t code);
static void logManualWrite(uint32_t value);

void setup()
{
    pinMode(PIN_STATUS, OUTPUT);
    digitalWrite(PIN_STATUS, LOW);

    Serial.begin(115200);
    const uint32_t serialStart = millis();
    while (!Serial && (millis() - serialStart) < 2000U) {
        // Wait briefly for USB serial on boards that need it.
    }

    freqCalc.RefClock1 = REF_MHZ;

#if !defined(SPECANN_CI_BUILD)
    halLo1.begin();
    halLo2.begin();
    halLo3.begin();

    pinMode(PIN_ATTEN, OUTPUT);
    pinMode(PIN_REF_EN1, OUTPUT);
    pinMode(PIN_REF_EN2, OUTPUT);

    digitalWrite(PIN_ATTEN, HIGH); // Idle high so the attenuator is not latched unintentionally.
    digitalWrite(PIN_REF_EN1, HIGH);
    digitalWrite(PIN_REF_EN2, LOW);

    initializeLo(lo1);
    initializeLo(lo2);
    initializeLo(lo3);

    programAttenuatorDb(currentAttenuatorDb);
    Serial.println(F("Note: Attenuator programming assumes PE43711 0.25 dB step codes—verify against hardware."));
#else
    Serial.println(F("SPECANN_CI_BUILD defined: hardware initialization skipped."));
#endif

    tuneTo(currentRfInputMhz);
    printBanner();
    printStatus();

    lastHeartbeatToggleMs = millis();
}

void loop()
{
    pollSerial();
    heartbeat();
}

static void printBanner()
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
    Serial.println(F("  ifmode <lo#> <high|low>  Set LO injection sense"));
    Serial.println(F("  chip <lo1|lo2|lo3|atten|aux>  Select manual SPI target"));
    Serial.println(F("  spi <hex32>        Send raw 32-bit word to selected device"));
    Serial.println();
}

static void tuneTo(double mhz)
{
    currentRfInputMhz = mhz;
    recomputePlan();
}

static void printFrequencyPlan()
{
    Serial.println(F("\nFrequency Plan"));
    Serial.print(F("RF In : ")); Serial.print(freqCalc.FreqRFin, 3); Serial.println(F(" MHz"));
    Serial.print(F("LO1  : ")); Serial.print(freqCalc.FreqLO1, 3);
    Serial.print(F(" MHz  IF1: ")); Serial.println(freqCalc.IF1, 3);
    Serial.print(F("LO2  : ")); Serial.print(freqCalc.FreqLO2, 3); Serial.println(F(" MHz"));
    Serial.print(F("LO3  : ")); Serial.print(freqCalc.FreqLO3, 3); Serial.println(F(" MHz"));
}

static void printLoSummary(const __FlashStringHelper* label, const MAX2871& lo)
{
    Serial.print(label);
    Serial.print(F(" M=")); Serial.print(lo.M);
    Serial.print(F(" F=")); Serial.print(lo.Frac);
    Serial.print(F(" N=")); Serial.print(lo.N);
    Serial.print(F(" DIVA=")); Serial.println(lo.DIVA);
}

static void printStatus()
{
    printFrequencyPlan();
    printLoSummary(F("LO1"), lo1);
    printLoSummary(F("LO2"), lo2);
    printLoSummary(F("LO3"), lo3);
    Serial.print(F("Injection: LO1="));
    Serial.print(injectionModeName(freqCalc.LO1InjectionMode));
    Serial.print(F(" LO2="));
    Serial.print(injectionModeName(freqCalc.LO2InjectionMode));
    Serial.print(F(" LO3="));
    Serial.println(injectionModeName(freqCalc.LO3InjectionMode));
    Serial.print(F("Attenuator: ")); Serial.print(currentAttenuatorDb, 2); Serial.println(F(" dB"));
    Serial.print(F("Manual target: "));
    Serial.println(chipTargetName(currentChipTarget));
}

static void pollSerial()
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
            // Buffer overflow, reset and notify.
            inputLength = 0;
            Serial.println(F("Input too long, line cleared."));
        }
    }
}

static void heartbeat()
{
    const uint32_t now = millis();
    if ((now - lastHeartbeatToggleMs) >= HEARTBEAT_INTERVAL_MS) {
        lastHeartbeatToggleMs = now;
        heartbeatState = !heartbeatState;
        digitalWrite(PIN_STATUS, heartbeatState ? HIGH : LOW);
    }
}

static bool equalsIgnoreCase(const char* lhs, const char* rhs)
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

static void trimWhitespace(char* text)
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

static void handleCommand(const char* line)
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
        if (count < 3U) {
            Serial.println(F("Usage: ifmode <lo1|lo2|lo3> <high|low>"));
            return;
        }
        handleIfmodeCommand(tokens[1], tokens[2]);
        return;
    }
    if (equalsIgnoreCase(tokens[0], "chip")) {
        if (count < 2U) {
            Serial.println(F("Usage: chip <lo1|lo2|lo3|atten|aux>"));
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

static void handleAttenuatorCommand(const char* valueToken)
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
    Serial.print(currentAttenuatorDb, 2);
    Serial.println(F(" dB"));
    Serial.println(F("Reminder: expect ~51 ohms at 31.75 dB."));
    printStatus();
}

static void handleIfmodeCommand(const char* loToken, const char* modeToken)
{
    if (loToken == nullptr || modeToken == nullptr) {
        return;
    }
    const bool highRequested = equalsIgnoreCase(modeToken, "high");
    const bool lowRequested = equalsIgnoreCase(modeToken, "low");
    if (!highRequested && !lowRequested) {
        Serial.println(F("ifmode requires 'high' or 'low'."));
        return;
    }
    const LOInjectionMode requestedMode = highRequested ? LOInjectionMode::High : LOInjectionMode::Low;
    if (equalsIgnoreCase(loToken, "lo1")) {
        desiredLo1Injection = requestedMode;
    } else if (equalsIgnoreCase(loToken, "lo2")) {
        desiredLo2Injection = requestedMode;
    } else if (equalsIgnoreCase(loToken, "lo3")) {
        desiredLo3Injection = requestedMode;
    } else {
        Serial.println(F("ifmode target must be lo1, lo2, or lo3."));
        return;
    }
    recomputePlan();
    Serial.print(F("IF mode updated for "));
    Serial.print(loToken);
    Serial.print(F(" -> "));
    Serial.println(highRequested ? F("HIGH-side injection") : F("LOW-side injection"));
    printStatus();
}

static void handleChipCommand(const char* targetToken)
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
    } else if (equalsIgnoreCase(targetToken, "aux")) {
        target = ChipTarget::Aux;
    } else {
        Serial.println(F("chip target must be lo1, lo2, lo3, atten, or aux."));
        return;
    }
    selectChip(target);
}

static void handleSpiCommand(const char* valueToken)
{
    if (valueToken == nullptr) {
        return;
    }
    if (currentChipTarget == ChipTarget::None) {
        Serial.println(F("No chip selected. Use 'chip <target>' first."));
        return;
    }
    char* endPointer = nullptr;
    const uint32_t value = static_cast<uint32_t>(strtoul(valueToken, &endPointer, 16));
    if (endPointer == nullptr || *endPointer != '\0') {
        Serial.println(F("SPI value must be hexadecimal (e.g., 0x12345678 or 12345678)."));
        return;
    }
    if (!manualSpiArmed) {
        if (!pendingSpiConfirmation) {
            pendingSpiConfirmation = true;
            pendingSpiValue = value;
            pendingSpiTarget = currentChipTarget;
            Serial.println(F("Manual SPI writes locked. Re-enter the same command to arm manual writes."));
            return;
        }
        if (pendingSpiValue != value || pendingSpiTarget != currentChipTarget) {
            pendingSpiValue = value;
            pendingSpiTarget = currentChipTarget;
            Serial.println(F("Confirmation mismatch. Re-enter desired value to arm manual writes."));
            return;
        }
        pendingSpiConfirmation = false;
        manualSpiArmed = true;
        Serial.println(F("Manual SPI writes armed. Proceed with caution."));
    }

    logManualWrite(value);

    switch (currentChipTarget) {
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
        case ChipTarget::Aux:
            Serial.println(F("Aux SPI target not yet wired; write skipped."));
            break;
        case ChipTarget::None:
        default:
            break;
    }
}

static void selectChip(ChipTarget target)
{
    if (target == currentChipTarget) {
        Serial.print(F("Manual target unchanged: "));
        Serial.println(chipTargetName(currentChipTarget));
        return;
    }
    deassertTarget(currentChipTarget);
    currentChipTarget = target;
    manualSpiArmed = false;
    pendingSpiConfirmation = false;
    Serial.print(F("Manual target set to "));
    Serial.println(chipTargetName(currentChipTarget));
}

static void deassertTarget(ChipTarget target)
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
        case ChipTarget::Aux:
        case ChipTarget::None:
        default:
            break;
    }
}

static const __FlashStringHelper* chipTargetName(ChipTarget target)
{
    switch (target) {
        case ChipTarget::LO1: return F("LO1");
        case ChipTarget::LO2: return F("LO2");
        case ChipTarget::LO3: return F("LO3");
        case ChipTarget::Attenuator: return F("Attenuator");
        case ChipTarget::Aux: return F("Auxiliary");
        case ChipTarget::None: default: return F("None");
    }
}

static const __FlashStringHelper* injectionModeName(LOInjectionMode mode)
{
    return (mode == LOInjectionMode::High) ? F("High") : F("Low");
}

static void programAttenuatorDb(double db)
{
    const uint8_t code = attenCodeFromDb(db);
    programAttenuatorRaw(code);
    currentAttenuatorDb = ATTEN_MIN_DB + (static_cast<double>(code) * ATTEN_STEP_DB);
}

static uint8_t attenCodeFromDb(double db)
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

static void programAttenuatorRaw(uint8_t code)
{
#if !defined(SPECANN_CI_BUILD)
    deassertTarget(ChipTarget::Attenuator);
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
        currentAttenuatorDb = mappedDb;
    }
}

static void recomputePlan()
{
    freqCalc.FreqRFin = currentRfInputMhz;
    freqCalc.R = 1;

    const double fpfd = freqCalc.RefClock1 / static_cast<double>(freqCalc.R);
    const double if1Step = fpfd * round(freqCalc.IF1_center / fpfd);
    const int sign = (desiredLo1Injection == LOInjectionMode::High) ? 1 : -1;

    freqCalc.LO1InjectionMode = desiredLo1Injection;
    freqCalc.FreqLO1 = fpfd * round((if1Step + static_cast<double>(sign) * currentRfInputMhz) / fpfd);
    freqCalc.IF1 = freqCalc.FreqLO1 - (static_cast<double>(sign) * currentRfInputMhz);
    if (freqCalc.IF1 < 0.0) {
        freqCalc.IF1 = fabs(freqCalc.IF1);
    }

    freqCalc.LO2InjectionMode = desiredLo2Injection;
    if (desiredLo2Injection == LOInjectionMode::High) {
        freqCalc.FreqLO2 = freqCalc.IF1 + freqCalc.IF2;
    } else {
        freqCalc.FreqLO2 = freqCalc.IF1 - freqCalc.IF2;
    }
    if (freqCalc.FreqLO2 < 0.0) {
        Serial.println(F("WARNING: Computed LO2 frequency negative. Check injection selection."));
        freqCalc.FreqLO2 = fabs(freqCalc.FreqLO2);
    }

    freqCalc.LO3InjectionMode = desiredLo3Injection;
    if (desiredLo3Injection == LOInjectionMode::High) {
        freqCalc.FreqLO3 = freqCalc.IF2 + freqCalc.IF3;
    } else {
        freqCalc.FreqLO3 = freqCalc.IF2 - freqCalc.IF3;
    }
    if (freqCalc.FreqLO3 < 0.0) {
        Serial.println(F("WARNING: Computed LO3 frequency negative. Check injection selection."));
        freqCalc.FreqLO3 = fabs(freqCalc.FreqLO3);
    }

#if !defined(SPECANN_CI_BUILD)
    lo1.setFrequency(freqCalc.FreqLO1);
    lo2.setFrequency(freqCalc.FreqLO2);
    lo3.setFrequency(freqCalc.FreqLO3);
#endif

#if !defined(SPECANN_CI_BUILD)
    freqCalc.FreqLO1 = lo1.fmn2freq();
    freqCalc.FreqLO2 = lo2.fmn2freq();
    freqCalc.FreqLO3 = lo3.fmn2freq();
#else
    Serial.println(F("(CI) Frequency plan recalculated (no hardware writes)."));
#endif
}

static void logManualWrite(uint32_t value)
{
    Serial.print(F("[SPI] target="));
    Serial.print(chipTargetName(currentChipTarget));
    Serial.print(F(" value=0x"));
    static const char hexDigits[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4) {
        const uint8_t nibble = static_cast<uint8_t>((value >> shift) & 0x0FU);
        Serial.print(hexDigits[nibble]);
    }
    Serial.println();
    Serial.println(F("WARNING: manual writes can damage hardware if misused."));
}
#else
int main()
{
    return 0;
}
#endif
