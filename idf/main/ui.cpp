#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

#include "fonts/FreeMonoBold18pt7b.h"
#include "fonts/FreeSans9pt7b.h"
#include "fonts/FreeSansBold24pt7b.h"
#include "fonts/FreeSansBold9pt7b.h"

using std::max;

// Last values shown on screen (change-detection baseline)
static uint16_t last_co2 = 0xFFFF, last_pm25 = 0xFFFF, last_pm10 = 0xFFFF, last_tvoc = 0xFFFF, last_hcho = 0xFFFF;
static float last_temp = -999.0f, last_hum = -999.0f;
static bool last_gas_ready = false;

// ===================================================================
// AIR QUALITY INDEX
// ===================================================================

int calculateIAQI(const SensorData& d)
{
    // PM2.5 (EAQI bands)
    int subPM25 = 1;
    if (d.pm25 > 50) subPM25 = 5;
    else if (d.pm25 > 25) subPM25 = 4;
    else if (d.pm25 > 20) subPM25 = 3;
    else if (d.pm25 > 10) subPM25 = 2;

    // PM10 (EAQI bands)
    int subPM10 = 1;
    if (d.pm10 > 100) subPM10 = 5;
    else if (d.pm10 > 50) subPM10 = 4;
    else if (d.pm10 > 40) subPM10 = 3;
    else if (d.pm10 > 20) subPM10 = 2;

    // Humidity: comfort/mould penalty, capped at B (3)
    int subHUM = 1;
    if (d.hum < 25 || d.hum > 70) subHUM = 3;
    else if (d.hum < 30 || d.hum > 60) subHUM = 2;

    int score = max(subPM25, max(subPM10, subHUM));

    // Gases only count once the sensor has warmed up
    if (gasReady()) {
        int subCO2 = 1;
        if (d.co2 > 1500) subCO2 = 5;
        else if (d.co2 > 1000) subCO2 = 4;
        else if (d.co2 > 800) subCO2 = 3;
        else if (d.co2 > 600) subCO2 = 2;

        int subTVOC = 1;
        if (d.tvoc > 3000) subTVOC = 5;
        else if (d.tvoc > 1000) subTVOC = 4;
        else if (d.tvoc > 500) subTVOC = 3;
        else if (d.tvoc > 300) subTVOC = 2;

        int subHCHO = 1;
        if (d.hcho > 200) subHCHO = 5;
        else if (d.hcho > 100) subHCHO = 4;
        else if (d.hcho > 60) subHCHO = 3;
        else if (d.hcho > 30) subHCHO = 2;

        score = max(score, max(subCO2, max(subTVOC, subHCHO)));
    }
    return score;
}

const char* getIAQIText(int score)
{
    switch (score) {
        case 1: return "A+";
        case 2: return "A";
        case 3: return "B";
        case 4: return "C";
        case 5: return "F";
        default: return "-";
    }
}

// ===================================================================
// CHANGE DETECTION
// ===================================================================

static bool co2Changed(uint16_t n, uint16_t o)
{
    if (o == 0xFFFF) return true;
    uint16_t diff = abs((int)n - (int)o);
    return diff >= (50 + (o * 0.03));
}

static bool hchoChanged(uint16_t n, uint16_t o)
{
    if (o == 0xFFFF) return true;
    uint16_t diff = abs((int)n - (int)o);
    return diff >= max((uint16_t)2, (uint16_t)(o * 0.10));
}

static bool tvocChanged(uint16_t n, uint16_t o)
{
    if (o == 0xFFFF) return true;
    uint16_t diff = abs((int)n - (int)o);
    return diff >= max((uint16_t)5, (uint16_t)(o * 0.25));
}

static bool pmChanged(uint16_t n, uint16_t o)
{
    if (o == 0xFFFF) return true;
    uint16_t diff = abs((int)n - (int)o);
    return diff >= max((uint16_t)10, (uint16_t)(o * 0.10));
}

static bool tempChanged(float n, float o)
{
    if (o < -900.0f) return true;
    return fabsf(n - o) >= 1.0f;
}

static bool humChanged(float n, float o)
{
    if (o < -900.0f) return true;
    return fabsf(n - o) >= 3.0f;
}

static void rememberShown(const SensorData& d, bool gasOk)
{
    last_temp = d.temp;
    last_hum = d.hum;
    last_co2 = d.co2;
    last_tvoc = d.tvoc;
    last_pm25 = d.pm25;
    last_pm10 = d.pm10;
    last_hcho = d.hcho;
    last_gas_ready = gasOk;
}

// ===================================================================
// DRAWING
// ===================================================================

static void drawCard(Epd& display, int16_t x, int16_t y, int16_t w, int16_t h,
                     const char* label, const char* valStr, const char* unitStr)
{
    display.drawRect(x, y, w, h, EPD_BLACK);
    display.fillRect(x, y + h - 5, w, 5, EPD_BLACK);

    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(EPD_BLACK);

    int16_t x1, y1;
    uint16_t lw, lh;

    if (label && strlen(label) > 0) {
        display.getTextBounds(label, 0, 0, &x1, &y1, &lw, &lh);
        display.setCursor(x + (w - lw) / 2, y + 20);
        display.print(label);
    }

    if (valStr && strlen(valStr) > 0) {
        display.setFont(&FreeSansBold24pt7b);
        display.getTextBounds(valStr, 0, 0, &x1, &y1, &lw, &lh);
        display.setCursor(x + (w - lw) / 2, y + 62);
        display.print(valStr);
    }

    if (unitStr && strlen(unitStr) > 0) {
        display.setFont(&FreeSans9pt7b);
        display.getTextBounds(unitStr, 0, 0, &x1, &y1, &lw, &lh);
        display.setCursor(x + w - lw - 6, y + h - 10);
        display.print(unitStr);
    }
}

static void renderDataPortrait(Epd& display, const SensorData& d)
{
    display.fillScreen(EPD_WHITE);

    const int cardW = 142, cardH = 95, gapX = 8, gapY = 5, startX = 4, startY = 4;
    char strBuf[16];

    // ROW 1: Climate
    snprintf(strBuf, sizeof(strBuf), "%.1f", d.temp);
    drawCard(display, startX, startY, cardW, cardH, "TEMP", strBuf, "C");

    snprintf(strBuf, sizeof(strBuf), "%.1f", d.hum);
    drawCard(display, startX + cardW + gapX, startY, cardW, cardH, "HUM", strBuf, "%");

    bool gasOk = gasReady();

    // ROW 2: Gases
    if (gasOk) snprintf(strBuf, sizeof(strBuf), "%d", d.co2);
    else snprintf(strBuf, sizeof(strBuf), "--");
    drawCard(display, startX, startY + (cardH + gapY), cardW, cardH, "CO2", strBuf, "ppm");

    if (gasOk) snprintf(strBuf, sizeof(strBuf), "%d", d.tvoc);
    else snprintf(strBuf, sizeof(strBuf), "--");
    drawCard(display, startX + cardW + gapX, startY + (cardH + gapY), cardW, cardH, "TVOC", strBuf, "ug/m3");

    // ROW 3: Particulates
    snprintf(strBuf, sizeof(strBuf), "%d", d.pm25);
    drawCard(display, startX, startY + (cardH + gapY) * 2, cardW, cardH, "PM2.5", strBuf, "ug/m3");

    snprintf(strBuf, sizeof(strBuf), "%d", d.pm10);
    drawCard(display, startX + cardW + gapX, startY + (cardH + gapY) * 2, cardW, cardH, "PM10", strBuf, "ug/m3");

    // ROW 4: Chemicals & Score
    if (gasOk) snprintf(strBuf, sizeof(strBuf), "%d", d.hcho);
    else snprintf(strBuf, sizeof(strBuf), "--");
    drawCard(display, startX, startY + (cardH + gapY) * 3, cardW, cardH, "HCHO", strBuf, "ug/m3");

    drawCard(display, startX + cardW + gapX, startY + (cardH + gapY) * 3, cardW, cardH,
             "IAQI", getIAQIText(calculateIAQI(d)), "SCORE");
}

void uiShowSplash(Epd& display, const char* text)
{
    display.setFont(&FreeMonoBold18pt7b);
    display.setTextColor(EPD_BLACK);
    display.fillScreen(EPD_WHITE);
    display.setCursor(10, 100);
    display.print(text);
    display.displayFull();
}

void uiUpdatePartial(Epd& display, const SensorData& d)
{
    bool gasOk = gasReady();
    bool needUpdate = tempChanged(d.temp, last_temp) || humChanged(d.hum, last_hum)
                      || pmChanged(d.pm25, last_pm25) || pmChanged(d.pm10, last_pm10)
                      || (gasOk != last_gas_ready)  // redraw when warm-up ends
                      || (gasOk && (co2Changed(d.co2, last_co2) || tvocChanged(d.tvoc, last_tvoc)
                                    || hchoChanged(d.hcho, last_hcho)));
    if (!needUpdate) return;

    rememberShown(d, gasOk);
    renderDataPortrait(display, d);
    display.displayPartial();
}

void uiUpdateFull(Epd& display, const SensorData& d)
{
    // Keep the change-detection baseline in sync with what is on screen
    rememberShown(d, gasReady());
    renderDataPortrait(display, d);
    display.displayFull();
}
