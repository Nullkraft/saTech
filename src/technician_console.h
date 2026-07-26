#pragma once

#include <Arduino.h>

#include "fulltest.h"

void printTechnicianBanner();
void handleTechnicianCommand(const char* line);
void pollTechnicianConsole();
void printInjectionSummary();
