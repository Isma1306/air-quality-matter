#pragma once

#include <stdint.h>
#include "epd.h"

struct SensorData {
    uint16_t co2, hcho, tvoc, pm25, pm10;
    float temp, hum;
};

// True once the gas sensor (CO2/TVOC/HCHO) has warmed up (implemented in app_main.cpp)
bool gasReady();

// Worst-of sub-index, 1 (A+) .. 5 (F)
int calculateIAQI(const SensorData& d);
const char* getIAQIText(int score);

void uiShowSplash(Epd& display, const char* text);
void uiUpdateFull(Epd& display, const SensorData& d);
void uiUpdatePartial(Epd& display, const SensorData& d);  // only refreshes if values changed enough
