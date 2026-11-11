// Minimal ESP-IDF driver for the GoodDisplay GDEY042T81 (SSD1683, 400x300 B/W)
// with a small Adafruit-GFX-compatible drawing/text API.
//
// Ported from GxEPD2 (GxEPD2_420_GDEY042T81 + GxEPD2_BW) by Jean-Marc Zingg,
// keeping the same command sequences, so refresh behaviour matches the
// Arduino build.
#pragma once

#include <stdint.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "fonts/gfxfont.h"

#define EPD_BLACK 0x0000
#define EPD_WHITE 0xFFFF

class Epd {
public:
    static constexpr int16_t WIDTH = 400;   // native panel size
    static constexpr int16_t HEIGHT = 300;

    Epd(gpio_num_t sck, gpio_num_t mosi, gpio_num_t cs, gpio_num_t dc, gpio_num_t rst, gpio_num_t busy);

    // Sets up SPI + GPIO and resets the panel (like GxEPD2 init(..., true, 2, false)).
    void init();

    // ---- Adafruit GFX-like drawing API (operates on the RAM framebuffer) ----
    void setRotation(uint8_t r) { m_rotation = r & 3; }
    int16_t width() const { return (m_rotation & 1) ? HEIGHT : WIDTH; }
    int16_t height() const { return (m_rotation & 1) ? WIDTH : HEIGHT; }

    void fillScreen(uint16_t color);
    void drawPixel(int16_t x, int16_t y, uint16_t color);
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

    void setFont(const GFXfont* f) { m_font = f; }
    void setTextColor(uint16_t c) { m_textColor = c; }
    void setCursor(int16_t x, int16_t y) { m_cursorX = x; m_cursorY = y; }
    void print(const char* str);
    void getTextBounds(const char* str, int16_t x, int16_t y, int16_t* x1, int16_t* y1, uint16_t* w, uint16_t* h);

    // ---- Panel refresh ----
    // Full refresh (GxEPD2 setFullWindow + firstPage/nextPage)
    void displayFull();
    // Fast partial refresh of the whole screen (GxEPD2 setPartialWindow(0,0,w,h))
    void displayPartial();

private:
    void writeCommand(uint8_t c);
    void writeData(uint8_t d);
    void writeDataBuf(const uint8_t* data, size_t len);
    void writeFill(uint8_t value, size_t len);
    void waitWhileBusy(const char* comment, uint32_t timeoutMs = 10000);
    void reset();

    void initDisplay();
    void setPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    void writeScreenBuffer(uint8_t command, uint8_t value);
    void writeImage(uint8_t command);
    void clearScreen(uint8_t value);
    void updateFull();
    void updatePart();
    void powerOff();
    void ensureInitialWrite();

    void drawChar(int16_t x, int16_t y, unsigned char c);

    gpio_num_t m_sck, m_mosi, m_cs, m_dc, m_rst, m_busy;
    spi_device_handle_t m_spi = nullptr;

    uint8_t m_buffer[WIDTH / 8 * HEIGHT];
    uint8_t m_rotation = 0;
    const GFXfont* m_font = nullptr;
    uint16_t m_textColor = EPD_BLACK;
    int16_t m_cursorX = 0, m_cursorY = 0;

    bool m_initDone = false;
    bool m_initialWrite = true;
    bool m_initialRefresh = true;
    bool m_powerIsOn = false;
};
