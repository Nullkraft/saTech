#include "SpecAnn.h"

SpecAnn::SpecAnn(const SpecAnnPins& pins, double refMHz)
    : pins_(pins),
      refMHz_(refMHz),
      halLo1_(pins.leLo1),
      halLo2_(pins.leLo2),
      halLo3_(pins.leLo3),
      lo1_(refMHz_, halLo1_),
      lo2_(refMHz_, halLo2_),
      lo3_(refMHz_, halLo3_),
      freqCalc_(lo1_, lo2_, lo3_) {
    freqCalc_.RefClock1 = refMHz_;
}

void SpecAnn::begin() {
    pinMode(pins_.status, OUTPUT);
    digitalWrite(pins_.status, LOW);

    Serial.begin(115200);

    halLo1_.begin();
    halLo2_.begin();
    halLo3_.begin();

    pinMode(pins_.atten, OUTPUT);
    pinMode(pins_.refEn1, OUTPUT);
    pinMode(pins_.refEn2, OUTPUT);

    digitalWrite(pins_.atten, LOW);
    digitalWrite(pins_.refEn1, HIGH);
    digitalWrite(pins_.refEn2, LOW);

    initializeLo(lo1_);
    initializeLo(lo2_);
    initializeLo(lo3_);

    freqCalc_.set_LO_frequencies(1735.113, freqCalc_.RefClock1, 1);

    Serial.println(F("SpecAnn startup LO plan:"));
    Serial.print(F("  LO1 = "));
    Serial.print(freqCalc_.FreqLO1, 3);
    Serial.println(F(" MHz"));
    Serial.print(F("  LO2 = "));
    Serial.print(freqCalc_.FreqLO2, 3);
    Serial.println(F(" MHz"));
    Serial.print(F("  LO3 = "));
    Serial.print(freqCalc_.FreqLO3, 3);
    Serial.println(F(" MHz"));
}

void SpecAnn::loop() {
    digitalWrite(pins_.status, HIGH);
    delay(1);
    digitalWrite(pins_.status, LOW);
    delay(1);
}

void SpecAnn::initializeLo(MAX2871& lo) {
    lo.begin();
    lo.outputSelect(RF_ALL);
    lo.outputPower(+5, RF_ALL);
}
