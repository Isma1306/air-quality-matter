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
#include "qrcode.h"

using std::max;

// Last values shown on screen (change-detection baseline)
static uint16_t last_co2 = 0xFFFF, last_pm25 = 0xFFFF, last_pm10 = 0xFFFF, last_tvoc = 0xFFFF, last_hcho = 0xFFFF;
static float last_temp = -999.0f, last_hum = -999.0f;
static bool last_gas_ready = false;

// ===================================================================
// AIR QUALITY INDEX
// ===================================================================

// Per-pollutant sub-indices, 1 (best) .. 5 (worst)
static int levelPM25(uint16_t v)
{
    // EAQI bands
    if (v > 50) return 5;
    if (v > 25) return 4;
    if (v > 20) return 3;
    if (v > 10) return 2;
    return 1;
}

static int levelPM10(uint16_t v)
{
    // EAQI bands
    if (v > 100) return 5;
    if (v > 50) return 4;
    if (v > 40) return 3;
    if (v > 20) return 2;
    return 1;
}

static int levelHum(float v)
{
    // Comfort/mould penalty, capped at B (3)
    if (v < 25 || v > 70) return 3;
    if (v < 30 || v > 60) return 2;
    return 1;
}

static int levelCO2(uint16_t v)
{
    if (v > 1500) return 5;
    if (v > 1000) return 4;
    if (v > 800) return 3;
    if (v > 600) return 2;
    return 1;
}

static int levelTVOC(uint16_t v)
{
    if (v > 3000) return 5;
    if (v > 1000) return 4;
    if (v > 500) return 3;
    if (v > 300) return 2;
    return 1;
}

static int levelHCHO(uint16_t v)
{
    if (v > 200) return 5;
    if (v > 100) return 4;
    if (v > 60) return 3;
    if (v > 30) return 2;
    return 1;
}

int calculateIAQI(const SensorData& d)
{
    int score = max(levelPM25(d.pm25), max(levelPM10(d.pm10), levelHum(d.hum)));

    // Gases only count once the sensor has warmed up
    if (gasReady()) {
        score = max(score, max(levelCO2(d.co2), max(levelTVOC(d.tvoc), levelHCHO(d.hcho))));
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

// Ordered-dither shade: 1 = lightest .. 5 = solid black
static bool shadePixel(int level, int px, int py)
{
    switch (level) {
        case 1: return false;                                // outline only
        case 2: return (px % 3 == 0) && (py % 3 == 0);       // ~11 %
        case 3: return ((px + py) & 1) == 0;                 // 50 % checker
        case 4: return !((px % 2 == 0) && (py % 2 == 0));    // ~75 %
        default: return true;                                // solid
    }
}

// Black (left) to white (right) bar: 5 squares at level 1 (best),
// one fewer from the white end for each worse level
static void drawLevelSquares(Epd& display, int16_t x, int16_t y, int level)
{
    const int16_t size = 11, gap = 4;
    const int count = 6 - level;
    for (int i = 1; i <= count; i++) {
        int16_t sx = x + (i - 1) * (size + gap);
        display.drawRect(sx, y, size, size, EPD_BLACK);
        for (int py = 1; py < size - 1; py++)
            for (int px = 1; px < size - 1; px++)
                if (shadePixel(6 - i, px, py)) display.drawPixel(sx + px, y + py, EPD_BLACK);
    }
}

// level 0 = no squares (no index for this value, or gas sensor warming up)
static void drawCard(Epd& display, int16_t x, int16_t y, int16_t w, int16_t h,
                     const char* label, const char* valStr, const char* unitStr, int level = 0)
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

    // Bottom-left, below the value, on the unit's line
    if (level > 0) drawLevelSquares(display, x + 6, y + h - 21, level);
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
    drawCard(display, startX + cardW + gapX, startY, cardW, cardH, "HUM", strBuf, "%", levelHum(d.hum));

    bool gasOk = gasReady();

    // ROW 2: Gases
    if (gasOk) snprintf(strBuf, sizeof(strBuf), "%d", d.co2);
    else snprintf(strBuf, sizeof(strBuf), "--");
    drawCard(display, startX, startY + (cardH + gapY), cardW, cardH, "CO2", strBuf, "ppm", gasOk ? levelCO2(d.co2) : 0);

    if (gasOk) snprintf(strBuf, sizeof(strBuf), "%d", d.tvoc);
    else snprintf(strBuf, sizeof(strBuf), "--");
    drawCard(display, startX + cardW + gapX, startY + (cardH + gapY), cardW, cardH, "TVOC", strBuf, "ug/m3", gasOk ? levelTVOC(d.tvoc) : 0);

    // ROW 3: Particulates
    snprintf(strBuf, sizeof(strBuf), "%d", d.pm25);
    drawCard(display, startX, startY + (cardH + gapY) * 2, cardW, cardH, "PM2.5", strBuf, "ug/m3", levelPM25(d.pm25));

    snprintf(strBuf, sizeof(strBuf), "%d", d.pm10);
    drawCard(display, startX + cardW + gapX, startY + (cardH + gapY) * 2, cardW, cardH, "PM10", strBuf, "ug/m3", levelPM10(d.pm10));

    // ROW 4: Chemicals & Score
    if (gasOk) snprintf(strBuf, sizeof(strBuf), "%d", d.hcho);
    else snprintf(strBuf, sizeof(strBuf), "--");
    drawCard(display, startX, startY + (cardH + gapY) * 3, cardW, cardH, "HCHO", strBuf, "ug/m3", gasOk ? levelHCHO(d.hcho) : 0);

    int iaqi = calculateIAQI(d);
    drawCard(display, startX + cardW + gapX, startY + (cardH + gapY) * 3, cardW, cardH,
             "IAQI", getIAQIText(iaqi), "SCORE", iaqi);
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

// esp_qrcode_generate() has no user-data pointer, so the target is passed via statics
static Epd* s_qrDisplay = nullptr;
static int16_t s_qrTop = 0, s_qrMaxSize = 0;

static void drawQr(esp_qrcode_handle_t qr)
{
    const int modules = esp_qrcode_get_size(qr);
    const int quiet = 2;  // white border in modules (the screen around it is white too)
    const int scale = s_qrMaxSize / (modules + 2 * quiet);
    const int size = modules * scale;
    const int x0 = (s_qrDisplay->width() - size) / 2;
    const int y0 = s_qrTop + (s_qrMaxSize - size) / 2;
    for (int y = 0; y < modules; y++)
        for (int x = 0; x < modules; x++)
            if (esp_qrcode_get_module(qr, x, y))
                s_qrDisplay->fillRect(x0 + x * scale, y0 + y * scale, scale, scale, EPD_BLACK);
}

static void printCentered(Epd& display, const GFXfont* font, const char* text, int16_t baseline)
{
    int16_t x1, y1;
    uint16_t w, h;
    display.setFont(font);
    display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((display.width() - w) / 2 - x1, baseline);
    display.print(text);
}

void uiShowPairing(Epd& display, const char* qrPayload, const char* manualCode)
{
    display.fillScreen(EPD_WHITE);
    display.setTextColor(EPD_BLACK);

    printCentered(display, &FreeSansBold9pt7b, "Scan to pair (Matter)", 24);

    s_qrDisplay = &display;
    s_qrTop = 32;
    s_qrMaxSize = 270;
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func = drawQr;
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_MED;
    esp_qrcode_generate(&cfg, qrPayload);

    printCentered(display, &FreeSans9pt7b, "or enter the code", 322);
    printCentered(display, &FreeMonoBold18pt7b, manualCode, 358);
    printCentered(display, &FreeSans9pt7b, "Hold BOOT 5 s to reset", 392);

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
