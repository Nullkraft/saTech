#pragma once

#include <Arduino.h>

void printTechnicianBanner();
void printFulltestPlanReport();
void handleTechnicianCommand(const char* line);
void pollTechnicianConsole();
void printInjectionSummary();
