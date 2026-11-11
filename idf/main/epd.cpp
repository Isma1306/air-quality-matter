#include "epd.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "epd";

static constexpr int SPI_CLOCK_HZ = 10 * 1000 * 1000;  // same as GxEPD2
static constexpr size_t SPI_CHUNK = 4000;

static inline void delayMs(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1); }

Epd::Epd(gpio_num_t sck, gpio_num_t mosi, gpio_num_t cs, gpio_num_t dc, gpio_num_t rst, gpio_num_t busy)
    : m_sck(sck), m_mosi(mosi), m_cs(cs), m_dc(dc), m_rst(rst), m_busy(busy)
{
    memset(m_buffer, 0xFF, sizeof(m_buffer));
}

void Epd::init()
{
    gpio_config_t out = {};
    out.pin_bit_mask = (1ULL << m_cs) | (1ULL << m_dc) | (1ULL << m_rst);
    out.mode = GPIO_MODE_OUTPUT;
    gpio_config(&out);
    gpio_set_level(m_cs, 1);
    gpio_set_level(m_dc, 1);
    gpio_set_level(m_rst, 1);

    gpio_config_t in = {};
    in.pin_bit_mask = (1ULL << m_busy);
    in.mode = GPIO_MODE_INPUT;
    gpio_config(&in);

    spi_bus_config_t bus = {};
    bus.mosi_io_num = m_mosi;
    bus.miso_io_num = -1;
    bus.sclk_io_num = m_sck;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = SPI_CHUNK;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {};
    dev.clock_speed_hz = SPI_CLOCK_HZ;
    dev.mode = 0;
    dev.spics_io_num = -1;  // CS driven manually, like GxEPD2
    dev.queue_size = 1;
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &m_spi));

    reset();
}

// -------------------------------------------------------------------
// Low level SPI
// -------------------------------------------------------------------

void Epd::writeCommand(uint8_t c)
{
    spi_transaction_t t = {};
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = c;
    gpio_set_level(m_dc, 0);
    gpio_set_level(m_cs, 0);
    spi_device_polling_transmit(m_spi, &t);
    gpio_set_level(m_cs, 1);
    gpio_set_level(m_dc, 1);
}

void Epd::writeData(uint8_t d)
{
    spi_transaction_t t = {};
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.tx_data[0] = d;
    gpio_set_level(m_cs, 0);
    spi_device_polling_transmit(m_spi, &t);
    gpio_set_level(m_cs, 1);
}

void Epd::writeDataBuf(const uint8_t* data, size_t len)
{
    gpio_set_level(m_cs, 0);
    while (len > 0) {
        size_t n = len > SPI_CHUNK ? SPI_CHUNK : len;
        spi_transaction_t t = {};
        t.length = n * 8;
        t.tx_buffer = data;
        spi_device_polling_transmit(m_spi, &t);
        data += n;
        len -= n;
    }
    gpio_set_level(m_cs, 1);
}

void Epd::writeFill(uint8_t value, size_t len)
{
    static uint8_t chunk[256];
    memset(chunk, value, sizeof(chunk));
    gpio_set_level(m_cs, 0);
    while (len > 0) {
        size_t n = len > sizeof(chunk) ? sizeof(chunk) : len;
        spi_transaction_t t = {};
        t.length = n * 8;
        t.tx_buffer = chunk;
        spi_device_polling_transmit(m_spi, &t);
        len -= n;
    }
    gpio_set_level(m_cs, 1);
}

void Epd::waitWhileBusy(const char* comment, uint32_t timeoutMs)
{
    delayMs(1);
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(m_busy) == 1) {  // busy level HIGH
        if ((esp_timer_get_time() - start) / 1000 > timeoutMs) {
            ESP_LOGW(TAG, "Busy Timeout! (%s)", comment);
            break;
        }
        delayMs(1);
    }
    ESP_LOGD(TAG, "%s : %lld us", comment, esp_timer_get_time() - start);
}

void Epd::reset()
{
    // Same as GxEPD2_EPD::_reset() with reset_duration = 2 (Waveshare "clever" reset)
    gpio_set_level(m_rst, 1);
    delayMs(10);
    gpio_set_level(m_rst, 0);
    delayMs(2);
    gpio_set_level(m_rst, 1);
    delayMs(10);
}

// -------------------------------------------------------------------
// SSD1683 sequences (from GxEPD2_420_GDEY042T81)
// -------------------------------------------------------------------

void Epd::initDisplay()
{
    delayMs(10);
    writeCommand(0x12);  // SWRESET
    delayMs(10);
    writeCommand(0x01);  // Set MUX as 300
    writeData(0x2B);
    writeData(0x01);
    writeData(0x00);
    writeCommand(0x3C);  // BorderWaveform
    writeData(0x01);
    writeCommand(0x18);  // Read built-in temperature sensor
    writeData(0x80);
    setPartialRamArea(0, 0, WIDTH, HEIGHT);
    m_initDone = true;
}

void Epd::setPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    writeCommand(0x11);  // RAM entry mode
    writeData(0x03);     // x increase, y increase
    writeCommand(0x44);
    writeData(x / 8);
    writeData((x + w - 1) / 8);
    writeCommand(0x45);
    writeData(y % 256);
    writeData(y / 256);
    writeData((y + h - 1) % 256);
    writeData((y + h - 1) / 256);
    writeCommand(0x4E);
    writeData(x / 8);
    writeCommand(0x4F);
    writeData(y % 256);
    writeData(y / 256);
}

void Epd::writeScreenBuffer(uint8_t command, uint8_t value)
{
    if (!m_initDone) initDisplay();
    setPartialRamArea(0, 0, WIDTH, HEIGHT);
    writeCommand(command);
    writeFill(value, sizeof(m_buffer));
}

void Epd::clearScreen(uint8_t value)
{
    writeScreenBuffer(0x26, value);  // previous
    writeScreenBuffer(0x24, value);  // current
    updateFull();
    m_initialRefresh = false;
    m_initialWrite = false;
}

void Epd::ensureInitialWrite()
{
    if (!m_initDone) initDisplay();
    if (m_initialWrite) clearScreen(0xFF);
}

void Epd::writeImage(uint8_t command)
{
    ensureInitialWrite();
    setPartialRamArea(0, 0, WIDTH, HEIGHT);
    writeCommand(command);
    writeDataBuf(m_buffer, sizeof(m_buffer));
}

void Epd::updateFull()
{
    writeCommand(0x21);  // Display Update Control
    writeData(0x40);     // bypass RED as 0
    writeData(0x00);     // single chip application
    // fast full update (GDEY042T81::useFastFullUpdate = true)
    writeCommand(0x1A);  // write to temperature register
    writeData(0x6E);
    writeCommand(0x22);
    writeData(0xD7);
    writeCommand(0x20);
    waitWhileBusy("updateFull");
    m_powerIsOn = false;
}

void Epd::updatePart()
{
    writeCommand(0x21);
    writeData(0x00);  // RED normal
    writeData(0x00);
    writeCommand(0x22);
    writeData(0xFC);
    writeCommand(0x20);
    waitWhileBusy("updatePart");
    m_powerIsOn = true;
}

void Epd::powerOff()
{
    if (m_powerIsOn) {
        writeCommand(0x22);
        writeData(0x83);
        writeCommand(0x20);
        waitWhileBusy("powerOff");
    }
    m_powerIsOn = false;
}

void Epd::displayFull()
{
    writeImage(0x26);  // previous
    writeImage(0x24);  // current
    updateFull();
    m_initialRefresh = false;
    writeImage(0x26);  // "again" so the next partial update has a clean base
    writeImage(0x24);
    powerOff();
}

void Epd::displayPartial()
{
    writeImage(0x24);
    if (m_initialRefresh) {
        updateFull();  // first update must be a full one
        m_initialRefresh = false;
    } else {
        setPartialRamArea(0, 0, WIDTH, HEIGHT);
        updatePart();
    }
    writeImage(0x26);
    writeImage(0x24);
}

// -------------------------------------------------------------------
// Drawing (GxEPD2_BW / Adafruit_GFX semantics)
// -------------------------------------------------------------------

void Epd::fillScreen(uint16_t color)
{
    memset(m_buffer, color ? 0xFF : 0x00, sizeof(m_buffer));
}

void Epd::drawPixel(int16_t x, int16_t y, uint16_t color)
{
    if (x < 0 || x >= width() || y < 0 || y >= height()) return;
    int16_t t;
    switch (m_rotation) {
        case 1: t = x; x = y; y = t; x = WIDTH - x - 1; break;
        case 2: x = WIDTH - x - 1; y = HEIGHT - y - 1; break;
        case 3: t = x; x = y; y = t; y = HEIGHT - y - 1; break;
        default: break;
    }
    uint32_t i = x / 8 + (uint32_t)y * (WIDTH / 8);
    if (color) m_buffer[i] |= (1 << (7 - x % 8));
    else m_buffer[i] &= ~(1 << (7 - x % 8));
}

void Epd::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    for (int16_t j = y; j < y + h; j++)
        for (int16_t i = x; i < x + w; i++) drawPixel(i, j, color);
}

void Epd::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    fillRect(x, y, w, 1, color);
    fillRect(x, y + h - 1, w, 1, color);
    fillRect(x, y, 1, h, color);
    fillRect(x + w - 1, y, 1, h, color);
}

void Epd::drawChar(int16_t x, int16_t y, unsigned char c)
{
    const GFXglyph* glyph = &m_font->glyph[c - m_font->first];
    const uint8_t* bitmap = m_font->bitmap;
    uint16_t bo = glyph->bitmapOffset;
    uint8_t w = glyph->width, h = glyph->height;
    int8_t xo = glyph->xOffset, yo = glyph->yOffset;
    uint8_t bits = 0, bit = 0;
    for (uint8_t yy = 0; yy < h; yy++) {
        for (uint8_t xx = 0; xx < w; xx++) {
            if (!(bit++ & 7)) bits = bitmap[bo++];
            if (bits & 0x80) drawPixel(x + xo + xx, y + yo + yy, m_textColor);
            bits <<= 1;
        }
    }
}

void Epd::print(const char* str)
{
    if (!m_font || !str) return;
    for (; *str; str++) {
        unsigned char c = (unsigned char)*str;
        if (c == '\n') {
            m_cursorX = 0;
            m_cursorY += m_font->yAdvance;
            continue;
        }
        if (c < m_font->first || c > m_font->last) continue;
        const GFXglyph* glyph = &m_font->glyph[c - m_font->first];
        if (glyph->width > 0 && glyph->height > 0) {
            // text wrap (Adafruit_GFX default)
            if (m_cursorX + glyph->xOffset + glyph->width > width()) {
                m_cursorX = 0;
                m_cursorY += m_font->yAdvance;
            }
            drawChar(m_cursorX, m_cursorY, c);
        }
        m_cursorX += glyph->xAdvance;
    }
}

void Epd::getTextBounds(const char* str, int16_t x, int16_t y, int16_t* x1, int16_t* y1, uint16_t* w, uint16_t* h)
{
    int16_t minx = 0x7FFF, miny = 0x7FFF, maxx = -1, maxy = -1;
    if (m_font && str) {
        for (; *str; str++) {
            unsigned char c = (unsigned char)*str;
            if (c < m_font->first || c > m_font->last) continue;
            const GFXglyph* g = &m_font->glyph[c - m_font->first];
            if (g->width > 0 && g->height > 0) {
                int16_t gx1 = x + g->xOffset, gy1 = y + g->yOffset;
                int16_t gx2 = gx1 + g->width - 1, gy2 = gy1 + g->height - 1;
                if (gx1 < minx) minx = gx1;
                if (gy1 < miny) miny = gy1;
                if (gx2 > maxx) maxx = gx2;
                if (gy2 > maxy) maxy = gy2;
            }
            x += g->xAdvance;
        }
    }
    *x1 = x; *y1 = y; *w = 0; *h = 0;
    if (maxx >= minx) { *x1 = minx; *w = maxx - minx + 1; }
    if (maxy >= miny) { *y1 = miny; *h = maxy - miny + 1; }
}
