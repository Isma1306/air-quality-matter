#include <GxEPD2_BW.h>
#include <HardwareSerial.h>
#include <SPI.h>
// Free Sans Fonts
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>

// Free Mono Font
#include <Fonts/FreeMonoBold18pt7b.h>

// Matter libraries
#include <Arduino.h>
#include <Matter.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_matter.h>
#include <esp_matter_attribute_utils.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app/clusters/air-quality-server/air-quality-server.h>

// Import cluster namespaces used by esp_matter
using namespace chip::app::Clusters;
static AirQuality::Instance* g_airQualityInstance = nullptr;

using namespace esp_matter;
using namespace esp_matter::cluster;
using namespace esp_matter::endpoint;

typedef cluster_t* (*ConcentrationCreateFn)(endpoint_t*, cluster::concentration_measurement::config_t*, uint8_t);

uint16_t airQualityEndpointId = 0;
MatterTemperatureSensor TemperatureSensor;
MatterHumiditySensor HumiditySensor;

// Board USER BUTTON pin - decommissioning button
const uint8_t buttonPin = BOOT_PIN;

// Button control - decommission the Matter Node
uint32_t button_time_stamp = 0;  // debouncing control
bool button_state = false;       // false = released | true = pressed
const uint32_t decommissioningTimeout = 5000;

// -------------------------------------------------------------------
// Pin Mapping for Waveshare ESP32-C6-Zero
// Header pins only. Avoids strapping pins (4, 5, 8, 9, 15),
// native USB (12, 13) and UART0 TX/RX (16, 17).
// -------------------------------------------------------------------
#define EPD_SCK_PIN  (22)  // EPD CLK
#define EPD_MOSI_PIN (21)  // EPD DIN
#define CS_PIN   (20)
#define DC_PIN   (19)
#define RES_PIN  (18)
#define BUSY_PIN (14)

#define M702_RX_PIN 0  // Connect to Sensor TXD
#define M702_TX_PIN 1  // Connect to Sensor RXD

// Matter MeasurementUnitEnum values
#define MEAS_UNIT_PPM  (0)
#define MEAS_UNIT_UGM3 (4)

// Initialize GxEPD2 Display Driver
GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT> display(
  GxEPD2_420_GDEY042T81(CS_PIN, DC_PIN, RES_PIN, BUSY_PIN));

// Refresh screen variables
int updateCounter = 0;
const int FULL_REFRESH_INTERVAL = 1000;
const unsigned long INTERVAL = 3000;
unsigned long previousMillis = 0;

// Last values
uint16_t last_co2 = 0xFFFF, last_pm25 = 0xFFFF, last_pm10 = 0xFFFF, last_tvoc = 0xFFFF, last_hcho = 0xFFFF;
float last_temp = -999.0, last_hum = -999.0;

// Gas sensor (CO2/TVOC/HCHO) warm-up: values are ignored and shown as "--"
const unsigned long GAS_WARMUP_MS = 5UL * 60UL * 1000UL;  // 5 minutes
bool last_gas_ready = false;
bool gasReady() { return millis() >= GAS_WARMUP_MS; }

// Initialize Serial for Sensor
HardwareSerial m702Serial(1);

// Sensor Data Buffer
uint8_t buffer[17];

// Forward Declarations
void updateMatterCO2(uint16_t endpoint_id, float co2_ppm);
void updateMatterHCHO(uint16_t endpoint_id, float hcho_val);
void updateMatterTVOC(uint16_t endpoint_id, float tvoc_val);
void updateMatterPM25(uint16_t endpoint_id, float pm25_val);
void updateMatterPM10(uint16_t endpoint_id, float pm10_val);
void updateMatterAirQuality2(uint16_t endpoint_id, uint16_t co2, uint16_t pm25, uint16_t pm10, uint16_t tvoc, uint16_t hcho, float hum);
void updateDisplayFull(uint16_t co2, uint16_t pm25, uint16_t pm10, uint16_t tvoc, uint16_t hcho, float temp, float hum);
void updateDisplayPartial(uint16_t co2, uint16_t pm25, uint16_t pm10, uint16_t tvoc, uint16_t hcho, float temp, float hum);

// -------------------------------------------------------------------
// Device identity shown in HomeKit (Manufacturer / Model). Max 32 chars.
// -------------------------------------------------------------------
const char* DEVICE_VENDOR_NAME  = "Ismael Carrea";
const char* DEVICE_PRODUCT_NAME = "Air Quality Sensor V1";

// Wraps the default provider and overrides only the vendor/product names.
class CustomDeviceInfoProvider : public chip::DeviceLayer::DeviceInstanceInfoProvider {
public:
  explicit CustomDeviceInfoProvider(chip::DeviceLayer::DeviceInstanceInfoProvider* base) : mBase(base) {}

  CHIP_ERROR GetVendorName(char* buf, size_t bufSize) override { return copyStr(DEVICE_VENDOR_NAME, buf, bufSize); }
  CHIP_ERROR GetProductName(char* buf, size_t bufSize) override { return copyStr(DEVICE_PRODUCT_NAME, buf, bufSize); }

  CHIP_ERROR GetVendorId(uint16_t& v) override { return mBase->GetVendorId(v); }
  CHIP_ERROR GetProductId(uint16_t& v) override { return mBase->GetProductId(v); }
  CHIP_ERROR GetPartNumber(char* b, size_t n) override { return mBase->GetPartNumber(b, n); }
  CHIP_ERROR GetProductURL(char* b, size_t n) override { return mBase->GetProductURL(b, n); }
  CHIP_ERROR GetProductLabel(char* b, size_t n) override { return mBase->GetProductLabel(b, n); }
  CHIP_ERROR GetSerialNumber(char* b, size_t n) override { return mBase->GetSerialNumber(b, n); }
  CHIP_ERROR GetManufacturingDate(uint16_t& y, uint8_t& m, uint8_t& d) override { return mBase->GetManufacturingDate(y, m, d); }
  CHIP_ERROR GetHardwareVersion(uint16_t& v) override { return mBase->GetHardwareVersion(v); }
  CHIP_ERROR GetHardwareVersionString(char* b, size_t n) override { return mBase->GetHardwareVersionString(b, n); }
  CHIP_ERROR GetRotatingDeviceIdUniqueId(chip::MutableByteSpan& s) override { return mBase->GetRotatingDeviceIdUniqueId(s); }

private:
  static CHIP_ERROR copyStr(const char* src, char* buf, size_t bufSize) {
    size_t len = strlen(src);
    if (len >= bufSize) return CHIP_ERROR_BUFFER_TOO_SMALL;
    memcpy(buf, src, len + 1);
    return CHIP_NO_ERROR;
  }
  chip::DeviceLayer::DeviceInstanceInfoProvider* mBase;
};

void addConcentrationCluster(endpoint_t* ep, ConcentrationCreateFn createFn, uint8_t unit, float minVal, float maxVal) {
  cluster::concentration_measurement::config_t cfg;
  cfg.measurement_medium = 0;  // Air
  cfg.feature_flags = cluster::concentration_measurement::feature::numeric_measurement::get_id();
  cfg.features.numeric_measurement.measurement_unit = unit;
  cfg.features.numeric_measurement.min_measured_value = nullable<float>(minVal);
  cfg.features.numeric_measurement.max_measured_value = nullable<float>(maxVal);
  cfg.delegate = nullptr;
  createFn(ep, &cfg, CLUSTER_FLAG_SERVER);
}

// ===================================================================
// SETUP & MAIN LOOP
// ===================================================================

void setup() {
Serial.begin(115200);
  delay(500);

  // 1. Initialize Display First (Splash Screen)
  SPI.begin(EPD_SCK_PIN, -1, EPD_MOSI_PIN, CS_PIN);
  display.init(115200, true, 2, false);
  display.setRotation(3);  // Portrait, flipped 180° for the new case (was 1)
  display.setFont(&FreeMonoBold18pt7b);
  display.setTextColor(GxEPD_BLACK);

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setCursor(10, 100);
    display.print("Init...");
  } while (display.nextPage());

  // 2. Initialize Peripherals
  m702Serial.begin(9600, SERIAL_8N1, M702_RX_PIN, M702_TX_PIN);
  pinMode(buttonPin, INPUT_PULLUP);

  // 3. Wi-Fi is NOT started here. Matter owns it:
  //    - not commissioned: advertises over BLE, the Home app sends the
  //      WiFi credentials during pairing and Matter stores them in NVS
  //    - commissioned: Matter reconnects to the stored network on boot
  Serial.println("[Matter] Initializing endpoints...");

  TemperatureSensor.begin(-25.00);
  HumiditySensor.begin(95.00);

  node_t* node = node::get();
  if (node) {
    static air_quality_sensor::config_t config;

    endpoint_t* aqEndpoint = air_quality_sensor::create(node, &config, ENDPOINT_FLAG_NONE, NULL);
    if (aqEndpoint) {
      airQualityEndpointId = endpoint::get_id(aqEndpoint);
      uint8_t flags = CLUSTER_FLAG_SERVER;

      // Concentration clusters. The M702 reports CO2 in ppm and everything
      // else in ug/m3, so each cluster advertises its real unit.
      addConcentrationCluster(aqEndpoint, cluster::carbon_dioxide_concentration_measurement::create,
                              MEAS_UNIT_PPM, 400.0f, 5000.0f);
      addConcentrationCluster(aqEndpoint, cluster::formaldehyde_concentration_measurement::create,
                              MEAS_UNIT_UGM3, 0.0f, 1000.0f);
      addConcentrationCluster(aqEndpoint, cluster::total_volatile_organic_compounds_concentration_measurement::create,
                              MEAS_UNIT_UGM3, 0.0f, 2000.0f);
      addConcentrationCluster(aqEndpoint, cluster::pm25_concentration_measurement::create,
                              MEAS_UNIT_UGM3, 0.0f, 999.0f);
      addConcentrationCluster(aqEndpoint, cluster::pm10_concentration_measurement::create,
                              MEAS_UNIT_UGM3, 0.0f, 1000.0f);

      // 6. Air Quality Summary Cluster
      cluster::air_quality::config_t air_quality_config;
      cluster::air_quality::create(aqEndpoint, &air_quality_config, flags);

      Serial.printf("[Matter] Air Quality Endpoint fully built with ID: %d\n", airQualityEndpointId);
    }

    // Start Matter Stack
    Matter.begin();

    {
      static CustomDeviceInfoProvider customInfo(chip::DeviceLayer::GetDeviceInstanceInfoProvider());
      chip::DeviceLayer::PlatformMgr().LockChipStack();
      chip::DeviceLayer::SetDeviceInstanceInfoProvider(&customInfo);
      chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    }

    if (!Matter.isDeviceCommissioned()) {
      Serial.println("\nMatter Node is not commissioned yet.");
      Serial.printf("Manual pairing code: %s\r\n", Matter.getManualPairingCode().c_str());
      Serial.printf("QR code URL: %s\r\n", Matter.getOnboardingQRCodeUrl().c_str());
    }
  }
}

void loop() {
  unsigned long currentMillis = millis();

  // -------------------------------------------------------------------
  // 1. DRAIN & PARSE SERIAL BUFFER (Runs continuously)
  // -------------------------------------------------------------------
  static uint16_t latest_co2 = 0, latest_hcho = 0, latest_tvoc = 0, latest_pm25 = 0, latest_pm10 = 0;
  static float latest_temp = 0.0, latest_hum = 0.0;
  static bool newSensorDataAvailable = false;

  while (m702Serial.available() >= 17) {
    if (m702Serial.read() == 0x3C) {
      if (m702Serial.peek() == 0x02) {
        buffer[0] = 0x3C;
        uint16_t checksum = 0x3C;

        for (int i = 1; i < 17; i++) {
          buffer[i] = m702Serial.read();
          if (i < 16) checksum += buffer[i];
        }

        if ((checksum & 0xFF) == buffer[16]) {
          latest_co2  = (buffer[2] << 8) | buffer[3];
          latest_hcho = (buffer[4] << 8) | buffer[5];
          latest_tvoc = (buffer[6] << 8) | buffer[7];
          latest_pm25 = (buffer[8] << 8) | buffer[9];
          latest_pm10 = (buffer[10] << 8) | buffer[11];
          latest_temp = (buffer[12] & 0x7F) + (buffer[13] / 100.0);
          if (buffer[12] & 0x80) latest_temp = -latest_temp;
          latest_hum  = buffer[14] + (buffer[15] / 100.0);

          newSensorDataAvailable = true; // Mark data as fresh
        } else {
          Serial.println("Warning: M702 Checksum Error!");
        }
      }
    }
  }

  // -------------------------------------------------------------------
  // 2. TIMED MATTER & DISPLAY UPDATES (Runs independently of Serial reads)
  // -------------------------------------------------------------------
  if (currentMillis - previousMillis >= INTERVAL) {
    previousMillis = currentMillis;

    if (newSensorDataAvailable) {
      // Update Matter Stack Attributes
      TemperatureSensor.setTemperature(latest_temp);
      HumiditySensor.setHumidity(latest_hum);

      // Gas values are unreliable during warm-up: leave them unreported
      if (gasReady()) {
        updateMatterCO2(airQualityEndpointId, (float)latest_co2);
        updateMatterHCHO(airQualityEndpointId, (float)latest_hcho);
        updateMatterTVOC(airQualityEndpointId, (float)latest_tvoc);
      }
      updateMatterPM25(airQualityEndpointId, (float)latest_pm25);
      updateMatterPM10(airQualityEndpointId, (float)latest_pm10);
      updateMatterAirQuality2(airQualityEndpointId, latest_co2, latest_pm25, latest_pm10, latest_tvoc, latest_hcho, latest_hum);

      // Refresh Display
      updateCounter++;
      if (updateCounter >= FULL_REFRESH_INTERVAL) {
        updateDisplayFull(latest_co2, latest_pm25, latest_pm10, latest_tvoc, latest_hcho, latest_temp, latest_hum);
        updateCounter = 0;
      } else {
        updateDisplayPartial(latest_co2, latest_pm25, latest_pm10, latest_tvoc, latest_hcho, latest_temp, latest_hum);
      }

      newSensorDataAvailable = false; // Reset flag until next valid frame
    }
  }

  // -------------------------------------------------------------------
  // 3. BUTTON DECOMMISSIONING
  // -------------------------------------------------------------------
  if (digitalRead(buttonPin) == LOW && !button_state) {
    button_time_stamp = millis();
    button_state = true;
  }

  if (digitalRead(buttonPin) == HIGH && button_state) {
    button_state = false;
  }

  if (button_state && (millis() - button_time_stamp > decommissioningTimeout)) {
    Serial.println("Decommissioning Air Sensor Matter Accessory...");
    Matter.decommission();
    button_time_stamp = millis();
  }

  // -------------------------------------------------------------------
  // 3b. WIFI POWER SAVE OFF (once per connection, for responsiveness)
  // -------------------------------------------------------------------
  static bool wifiPsApplied = false;
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiPsApplied) {
      esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
      Serial.printf("[WiFi] Connected to '%s' (IP %s), power save off: %s\n",
                    WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                    err == ESP_OK ? "ok" : esp_err_to_name(err));
      wifiPsApplied = true;
    }
  } else {
    wifiPsApplied = false;
  }

  // -------------------------------------------------------------------
  // 4. NON-BLOCKING YIELD
  // -------------------------------------------------------------------
  vTaskDelay(pdMS_TO_TICKS(10)); // Gives priority back to the Matter background task
}

// ===================================================================
// HELPER FUNCTIONS
// ===================================================================

// Worst-of sub-index, 1 (A+) .. 5 (F)
int calculateIAQI(uint16_t co2, uint16_t pm25, uint16_t pm10, uint16_t tvoc, uint16_t hcho, float hum) {
  // PM2.5 (EAQI bands)
  int subPM25 = 1;
  if (pm25 > 50) subPM25 = 5;
  else if (pm25 > 25) subPM25 = 4;
  else if (pm25 > 20) subPM25 = 3;
  else if (pm25 > 10) subPM25 = 2;

  // PM10 (EAQI bands)
  int subPM10 = 1;
  if (pm10 > 100) subPM10 = 5;
  else if (pm10 > 50) subPM10 = 4;
  else if (pm10 > 40) subPM10 = 3;
  else if (pm10 > 20) subPM10 = 2;

  // Humidity: comfort/mould penalty, capped at B (3)
  int subHUM = 1;
  if (hum < 25 || hum > 70) subHUM = 3;
  else if (hum < 30 || hum > 60) subHUM = 2;

  int score = max(subPM25, max(subPM10, subHUM));

  // Gases only count once the sensor has warmed up
  if (gasReady()) {
    int subCO2 = 1;
    if (co2 > 1500) subCO2 = 5;
    else if (co2 > 1000) subCO2 = 4;
    else if (co2 > 800) subCO2 = 3;
    else if (co2 > 600) subCO2 = 2;

    int subTVOC = 1;
    if (tvoc > 3000) subTVOC = 5;
    else if (tvoc > 1000) subTVOC = 4;
    else if (tvoc > 500) subTVOC = 3;
    else if (tvoc > 300) subTVOC = 2;

    int subHCHO = 1;
    if (hcho > 200) subHCHO = 5;
    else if (hcho > 100) subHCHO = 4;
    else if (hcho > 60) subHCHO = 3;
    else if (hcho > 30) subHCHO = 2;

    score = max(score, max(subCO2, max(subTVOC, subHCHO)));
  }

  return score;
}

const char* getIAQIText(int score) {
  switch (score) {
    case 1: return "A+";
    case 2: return "A";
    case 3: return "B";
    case 4: return "C";
    case 5: return "F";
    default: return "-";
  }
}

bool co2Changed(uint16_t newCo2, uint16_t oldCo2) {
  if (oldCo2 == 0xFFFF) return true;
  uint16_t diff = abs((int)newCo2 - (int)oldCo2);
  return diff >= (50 + (oldCo2 * 0.03));
}

bool hchoChanged(uint16_t newHcho, uint16_t oldHcho) {
  if (oldHcho == 0xFFFF) return true;
  uint16_t diff = abs((int)newHcho - (int)oldHcho);
  return diff >= max((uint16_t)2, (uint16_t)(oldHcho * 0.10));
}

bool tvocChanged(uint16_t newTvoc, uint16_t oldTvoc) {
  if (oldTvoc == 0xFFFF) return true;
  uint16_t diff = abs((int)newTvoc - (int)oldTvoc);
  return diff >= max((uint16_t)5, (uint16_t)(oldTvoc * 0.25));
}

bool pmChanged(uint16_t newPm, uint16_t oldPm) {
  if (oldPm == 0xFFFF) return true;
  uint16_t diff = abs((int)newPm - (int)oldPm);
  return diff >= max((uint16_t)10, (uint16_t)(oldPm * 0.10));
}

bool tempChanged(float newTemp, float oldTemp) {
  if (oldTemp < -900.0) return true;
  return abs(newTemp - oldTemp) >= 1.0;
}

bool humChanged(float newHum, float oldHum) {
  if (oldHum < -900.0) return true;
  return abs(newHum - oldHum) >= 3.0;
}

// ===================================================================
// UI DRAWING FUNCTIONS
// ===================================================================

void drawCard(int16_t x, int16_t y, int16_t w, int16_t h, const char* label, const char* valStr, const char* unitStr) {
  display.drawRect(x, y, w, h, GxEPD_BLACK);
  display.fillRect(x, y + h - 5, w, 5, GxEPD_BLACK);

  display.setFont(&FreeSansBold9pt7b);
  display.setTextColor(GxEPD_BLACK);

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

    int16_t unitX = x + w - lw - 6;
    int16_t unitY = y + h - 10;

    display.setCursor(unitX, unitY);
    display.print(unitStr);
  }
}

void renderDataPortrait(uint16_t co2, uint16_t pm25, uint16_t pm10, uint16_t tvoc, uint16_t hcho, float temp, float hum) {
  display.fillScreen(GxEPD_WHITE);

  int cardW = 142;
  int cardH = 95;
  int gapX = 8;
  int gapY = 5;
  int startX = 4;
  int startY = 4;

  char strBuf[16];

  // ROW 1: Climate
  snprintf(strBuf, sizeof(strBuf), "%.1f", temp);
  drawCard(startX, startY, cardW, cardH, "TEMP", strBuf, "C");

  snprintf(strBuf, sizeof(strBuf), "%.1f", hum);
  drawCard(startX + cardW + gapX, startY, cardW, cardH, "HUM", strBuf, "%");

  bool gasOk = gasReady();

  // ROW 2: Gases
  if (gasOk) snprintf(strBuf, sizeof(strBuf), "%d", co2);
  else snprintf(strBuf, sizeof(strBuf), "--");
  drawCard(startX, startY + (cardH + gapY), cardW, cardH, "CO2", strBuf, "ppm");

  if (gasOk) snprintf(strBuf, sizeof(strBuf), "%d", tvoc);
  else snprintf(strBuf, sizeof(strBuf), "--");
  drawCard(startX + cardW + gapX, startY + (cardH + gapY), cardW, cardH, "TVOC", strBuf, "ug/m3");

  // ROW 3: Particulates
  snprintf(strBuf, sizeof(strBuf), "%d", pm25);
  drawCard(startX, startY + (cardH + gapY) * 2, cardW, cardH, "PM2.5", strBuf, "ug/m3");

  snprintf(strBuf, sizeof(strBuf), "%d", pm10);
  drawCard(startX + cardW + gapX, startY + (cardH + gapY) * 2, cardW, cardH, "PM10", strBuf, "ug/m3");

  // ROW 4: Chemicals & Score
  if (gasOk) snprintf(strBuf, sizeof(strBuf), "%d", hcho);
  else snprintf(strBuf, sizeof(strBuf), "--");
  drawCard(startX, startY + (cardH + gapY) * 3, cardW, cardH, "HCHO", strBuf, "ug/m3");

  int iaqiScore = calculateIAQI(co2, pm25, pm10, tvoc, hcho, hum);
  const char* iaqiStatus = getIAQIText(iaqiScore);
  drawCard(startX + cardW + gapX, startY + (cardH + gapY) * 3, cardW, cardH, "IAQI", iaqiStatus, "SCORE");
}

// ===================================================================
// REFRESH & MATTER UPDATES
// ===================================================================

void updateDisplayPartial(uint16_t co2, uint16_t pm25, uint16_t pm10, uint16_t tvoc, uint16_t hcho, float temp, float hum) {
  bool gasOk = gasReady();
  bool needUpdate = tempChanged(temp, last_temp) || humChanged(hum, last_hum)
                    || pmChanged(pm25, last_pm25) || pmChanged(pm10, last_pm10)
                    || (gasOk != last_gas_ready)  // redraw when warm-up ends
                    || (gasOk && (co2Changed(co2, last_co2) || tvocChanged(tvoc, last_tvoc) || hchoChanged(hcho, last_hcho)));

  if (!needUpdate) {
    return;
  }

  last_temp = temp;
  last_hum = hum;
  last_co2 = co2;
  last_tvoc = tvoc;
  last_pm25 = pm25;
  last_pm10 = pm10;
  last_hcho = hcho;
  last_gas_ready = gasOk;

  display.setPartialWindow(0, 0, display.width(), display.height());
  display.firstPage();
  do {
    renderDataPortrait(co2, pm25, pm10, tvoc, hcho, temp, hum);
  } while (display.nextPage());
}

void updateDisplayFull(uint16_t co2, uint16_t pm25, uint16_t pm10, uint16_t tvoc, uint16_t hcho, float temp, float hum) {
  // Keep the change-detection baseline in sync with what is on screen
  last_temp = temp;
  last_hum = hum;
  last_co2 = co2;
  last_tvoc = tvoc;
  last_pm25 = pm25;
  last_pm10 = pm10;
  last_hcho = hcho;
  last_gas_ready = gasReady();

  display.setFullWindow();
  display.firstPage();
  do {
    renderDataPortrait(co2, pm25, pm10, tvoc, hcho, temp, hum);
  } while (display.nextPage());
}

void updateMatterCO2(uint16_t endpoint_id, float co2_ppm) {
  uint32_t cluster_id = chip::app::Clusters::CarbonDioxideConcentrationMeasurement::Id;
  uint32_t attribute_id = chip::app::Clusters::CarbonDioxideConcentrationMeasurement::Attributes::MeasuredValue::Id;
  esp_matter_attr_val_t matter_val = esp_matter_nullable_float(co2_ppm);
  esp_matter::attribute::update(endpoint_id, cluster_id, attribute_id, &matter_val);
}

void updateMatterHCHO(uint16_t endpoint_id, float hcho_val) {
  uint32_t cluster_id = chip::app::Clusters::FormaldehydeConcentrationMeasurement::Id;
  uint32_t attribute_id = chip::app::Clusters::FormaldehydeConcentrationMeasurement::Attributes::MeasuredValue::Id;
  esp_matter_attr_val_t matter_val = esp_matter_nullable_float(hcho_val);
  esp_matter::attribute::update(endpoint_id, cluster_id, attribute_id, &matter_val);
}

void updateMatterTVOC(uint16_t endpoint_id, float tvoc_val) {
  uint32_t cluster_id = chip::app::Clusters::TotalVolatileOrganicCompoundsConcentrationMeasurement::Id;
  uint32_t attribute_id = chip::app::Clusters::TotalVolatileOrganicCompoundsConcentrationMeasurement::Attributes::MeasuredValue::Id;
  esp_matter_attr_val_t matter_val = esp_matter_nullable_float(tvoc_val);
  esp_matter::attribute::update(endpoint_id, cluster_id, attribute_id, &matter_val);
}

void updateMatterPM25(uint16_t endpoint_id, float pm25_val) {
  uint32_t cluster_id = chip::app::Clusters::Pm25ConcentrationMeasurement::Id;
  uint32_t attribute_id = chip::app::Clusters::Pm25ConcentrationMeasurement::Attributes::MeasuredValue::Id;
  esp_matter_attr_val_t matter_val = esp_matter_nullable_float(pm25_val);
  esp_matter::attribute::update(endpoint_id, cluster_id, attribute_id, &matter_val);
}

void updateMatterPM10(uint16_t endpoint_id, float pm10_val) {
  uint32_t cluster_id = chip::app::Clusters::Pm10ConcentrationMeasurement::Id;
  uint32_t attribute_id = chip::app::Clusters::Pm10ConcentrationMeasurement::Attributes::MeasuredValue::Id;
  esp_matter_attr_val_t matter_val = esp_matter_nullable_float(pm10_val);
  esp_matter::attribute::update(endpoint_id, cluster_id, attribute_id, &matter_val);
}

void updateMatterAirQuality2(uint16_t endpoint_id, uint16_t co2, uint16_t pm25, uint16_t pm10, uint16_t tvoc, uint16_t hcho, float hum) {
  if (g_airQualityInstance == nullptr) {
    using namespace chip;
    using namespace chip::app::Clusters::AirQuality;

    BitMask<AirQuality::Feature> features(
      AirQuality::Feature::kFair,
      AirQuality::Feature::kModerate,
      AirQuality::Feature::kVeryPoor,
      AirQuality::Feature::kExtremelyPoor);

    chip::DeviceLayer::PlatformMgr().LockChipStack();

    g_airQualityInstance = new Instance(
      EndpointId(endpoint_id),
      features);

    if (g_airQualityInstance != nullptr) {
      g_airQualityInstance->Init();
      Serial.println("[Matter] CHIP Air Quality Instance lazily initialized!");
    }

    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
  }

  if (g_airQualityInstance == nullptr) return;

  int iaqiScore = calculateIAQI(co2, pm25, pm10, tvoc, hcho, hum);

  chip::app::Clusters::AirQuality::AirQualityEnum aqEnum;
  switch (iaqiScore) {
    case 1: aqEnum = chip::app::Clusters::AirQuality::AirQualityEnum::kGood; break;
    case 2: aqEnum = chip::app::Clusters::AirQuality::AirQualityEnum::kFair; break;
    case 3: aqEnum = chip::app::Clusters::AirQuality::AirQualityEnum::kModerate; break;
    case 4: aqEnum = chip::app::Clusters::AirQuality::AirQualityEnum::kPoor; break;
    case 5: aqEnum = chip::app::Clusters::AirQuality::AirQualityEnum::kVeryPoor; break;
    default: aqEnum = chip::app::Clusters::AirQuality::AirQualityEnum::kUnknown; break;
  }

  chip::DeviceLayer::PlatformMgr().LockChipStack();
  g_airQualityInstance->UpdateAirQuality(aqEnum);
  chip::DeviceLayer::PlatformMgr().UnlockChipStack();
}