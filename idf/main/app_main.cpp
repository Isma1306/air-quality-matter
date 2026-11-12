// Air Quality Sensor V1 - ESP-IDF + esp-matter port of sensorMonitor_v1.ino
//
// Board : Waveshare ESP32-C6-Zero
// Sensor: M702 (CO2 / HCHO / TVOC / PM2.5 / PM10 / Temp / Hum) on UART1
// Screen: GoodDisplay GDEY042T81 4.2" e-paper (SSD1683)
//
// Matter endpoints (same order as the Arduino build):
//   1 = Temperature Sensor, 2 = Humidity Sensor, 3 = Air Quality Sensor
//
// Network: Matter over Thread (BLE only for commissioning).
// Pairing data (passcode, discriminator, DAC, vendor/product names, serial)
// is per device and lives in the "fctry" partition: see factory/gen_factory.sh.

#include <atomic>
#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <esp_matter.h>
#include <esp_matter_attribute_utils.h>

#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/clusters/air-quality-server/air-quality-server.h>
#include <app/server/Server.h>
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <esp_openthread_types.h>
#include <platform/ESP32/OpenthreadLauncher.h>
#endif
#include <setup_payload/OnboardingCodesUtil.h>

#include "epd.h"
#include "ui.h"

static const char* TAG = "app";

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

// -------------------------------------------------------------------
// Pin Mapping for Waveshare ESP32-C6-Zero
// Header pins only. Avoids strapping pins (4, 5, 8, 9, 15),
// native USB (12, 13) and UART0 TX/RX (16, 17).
// -------------------------------------------------------------------
#define EPD_SCK_PIN  GPIO_NUM_22  // EPD CLK
#define EPD_MOSI_PIN GPIO_NUM_21  // EPD DIN
#define EPD_CS_PIN   GPIO_NUM_20
#define EPD_DC_PIN   GPIO_NUM_19
#define EPD_RES_PIN  GPIO_NUM_18
#define EPD_BUSY_PIN GPIO_NUM_14

#define M702_UART    UART_NUM_1
#define M702_RX_PIN  GPIO_NUM_0  // Connect to Sensor TXD
#define M702_TX_PIN  GPIO_NUM_1  // Connect to Sensor RXD

// Board BOOT button - hold 5 s to decommission (factory reset)
#define BUTTON_PIN   GPIO_NUM_9
static const uint32_t DECOMMISSIONING_TIMEOUT_MS = 5000;

// Matter MeasurementUnitEnum values
#define MEAS_UNIT_PPM  (0)
#define MEAS_UNIT_UGM3 (4)

// Refresh timing
static const int FULL_REFRESH_INTERVAL = 1000;  // partial updates between full refreshes
static const uint32_t INTERVAL_MS = 3000;

// Gas sensor (CO2/TVOC/HCHO) warm-up: values are ignored and shown as "--"
static const uint32_t GAS_WARMUP_MS = 5UL * 60UL * 1000UL;  // 5 minutes

static uint16_t temperatureEndpointId = 0;
static uint16_t humidityEndpointId = 0;
static uint16_t airQualityEndpointId = 0;
static AirQuality::Instance* g_airQualityInstance = nullptr;

static Epd display(EPD_SCK_PIN, EPD_MOSI_PIN, EPD_CS_PIN, EPD_DC_PIN, EPD_RES_PIN, EPD_BUSY_PIN);

static inline uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }

bool gasReady() { return millis() >= GAS_WARMUP_MS; }

// Set from the Matter task, handled in the main loop (which owns the display)
static std::atomic<bool> s_commissioned{false};
static std::atomic<bool> s_redrawRequested{false};
static std::atomic<bool> s_resetRequested{false};

// ===================================================================
// MATTER CALLBACKS
// ===================================================================

static void app_event_cb(const ChipDeviceEvent* event, intptr_t arg)
{
    switch (event->Type) {
        case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
            ESP_LOGI(TAG, "Commissioning complete");
            s_commissioned = true;
            s_redrawRequested = true;  // replace the QR code with the sensor data
            break;
        case chip::DeviceLayer::DeviceEventType::kThreadConnectivityChange:
            ESP_LOGI(TAG, "[Thread] %s",
                     event->ThreadConnectivityChange.Result == chip::DeviceLayer::kConnectivity_Established
                         ? "attached to the Thread network" : "detached from the Thread network");
            break;
        case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
            ESP_LOGW(TAG, "Commissioning failed, fail safe timer expired");
            break;
        case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
            ESP_LOGI(TAG, "Fabric removed");
            if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
                // Removed from the last controller: factory reset so the device
                // leaves the Thread network and advertises over BLE with the QR
                // code on screen again. Done from the main loop after a short
                // delay, so the RemoveFabric response still goes out.
                ESP_LOGW(TAG, "Last fabric removed, factory resetting");
                s_resetRequested = true;
            }
            break;
        default:
            break;
    }
}

// Sensors are read-only, nothing to handle
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t* val, void* priv_data)
{
    return ESP_OK;
}

static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void* priv_data)
{
    ESP_LOGI(TAG, "Identify request on endpoint %u", endpoint_id);
    return ESP_OK;
}

// ===================================================================
// MATTER DATA MODEL
// ===================================================================

typedef cluster_t* (*ConcentrationCreateFn)(endpoint_t*, cluster::concentration_measurement::config_t*, uint8_t);

static void addConcentrationCluster(endpoint_t* ep, ConcentrationCreateFn createFn, uint8_t unit, float minVal, float maxVal)
{
    cluster::concentration_measurement::config_t cfg;
    cfg.measurement_medium = 0;  // Air
    cfg.feature_flags = cluster::concentration_measurement::feature::numeric_measurement::get_id();
    cfg.features.numeric_measurement.measurement_unit = unit;
    cfg.features.numeric_measurement.min_measured_value = nullable<float>(minVal);
    cfg.features.numeric_measurement.max_measured_value = nullable<float>(maxVal);
    cfg.delegate = nullptr;
    createFn(ep, &cfg, CLUSTER_FLAG_SERVER);
}

static void createMatterNode()
{
    static node::config_t node_config;
    node_t* node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        abort();
    }

    // 1. Temperature sensor (value unknown until the first reading)
    static temperature_sensor::config_t temp_config;
    endpoint_t* tempEp = temperature_sensor::create(node, &temp_config, ENDPOINT_FLAG_NONE, NULL);
    temperatureEndpointId = endpoint::get_id(tempEp);

    // 2. Humidity sensor
    static humidity_sensor::config_t hum_config;
    endpoint_t* humEp = humidity_sensor::create(node, &hum_config, ENDPOINT_FLAG_NONE, NULL);
    humidityEndpointId = endpoint::get_id(humEp);

    // 3. Air quality sensor (includes the Air Quality cluster)
    static air_quality_sensor::config_t aq_config;
    endpoint_t* aqEp = air_quality_sensor::create(node, &aq_config, ENDPOINT_FLAG_NONE, NULL);
    airQualityEndpointId = endpoint::get_id(aqEp);

    // Concentration clusters. The M702 reports CO2 in ppm and everything
    // else in ug/m3, so each cluster advertises its real unit.
    addConcentrationCluster(aqEp, cluster::carbon_dioxide_concentration_measurement::create, MEAS_UNIT_PPM, 400.0f, 5000.0f);
    addConcentrationCluster(aqEp, cluster::formaldehyde_concentration_measurement::create, MEAS_UNIT_UGM3, 0.0f, 1000.0f);
    addConcentrationCluster(aqEp, cluster::total_volatile_organic_compounds_concentration_measurement::create, MEAS_UNIT_UGM3, 0.0f, 2000.0f);
    addConcentrationCluster(aqEp, cluster::pm25_concentration_measurement::create, MEAS_UNIT_UGM3, 0.0f, 999.0f);
    addConcentrationCluster(aqEp, cluster::pm10_concentration_measurement::create, MEAS_UNIT_UGM3, 0.0f, 1000.0f);

    ESP_LOGI(TAG, "Endpoints: temp=%u hum=%u air_quality=%u", temperatureEndpointId, humidityEndpointId, airQualityEndpointId);
}

// Must be called after esp_matter::start()
static void initAirQualityInstance()
{
    using namespace chip::app::Clusters::AirQuality;
    chip::BitMask<Feature> features(Feature::kFair, Feature::kModerate, Feature::kVeryPoor, Feature::kExtremelyPoor);

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    g_airQualityInstance = new Instance(chip::EndpointId(airQualityEndpointId), features);
    CHIP_ERROR err = g_airQualityInstance->Init();
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Air Quality instance init failed: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

static void updateConcentration(uint32_t cluster_id, float value)
{
    // All concentration clusters share the MeasuredValue attribute id (0x0000)
    esp_matter_attr_val_t val = esp_matter_nullable_float(value);
    attribute::update(airQualityEndpointId, cluster_id,
                      CarbonDioxideConcentrationMeasurement::Attributes::MeasuredValue::Id, &val);
}

static void updateMatter(const SensorData& d)
{
    esp_matter_attr_val_t t = esp_matter_nullable_int16((int16_t)lroundf(d.temp * 100.0f));
    attribute::update(temperatureEndpointId, TemperatureMeasurement::Id,
                      TemperatureMeasurement::Attributes::MeasuredValue::Id, &t);

    esp_matter_attr_val_t h = esp_matter_nullable_uint16((uint16_t)lroundf(d.hum * 100.0f));
    attribute::update(humidityEndpointId, RelativeHumidityMeasurement::Id,
                      RelativeHumidityMeasurement::Attributes::MeasuredValue::Id, &h);

    // Gas values are unreliable during warm-up: leave them unreported
    if (gasReady()) {
        updateConcentration(CarbonDioxideConcentrationMeasurement::Id, d.co2);
        updateConcentration(FormaldehydeConcentrationMeasurement::Id, d.hcho);
        updateConcentration(TotalVolatileOrganicCompoundsConcentrationMeasurement::Id, d.tvoc);
    }
    updateConcentration(Pm25ConcentrationMeasurement::Id, d.pm25);
    updateConcentration(Pm10ConcentrationMeasurement::Id, d.pm10);

    if (g_airQualityInstance) {
        AirQuality::AirQualityEnum aq;
        switch (calculateIAQI(d)) {
            case 1: aq = AirQuality::AirQualityEnum::kGood; break;
            case 2: aq = AirQuality::AirQualityEnum::kFair; break;
            case 3: aq = AirQuality::AirQualityEnum::kModerate; break;
            case 4: aq = AirQuality::AirQualityEnum::kPoor; break;
            case 5: aq = AirQuality::AirQualityEnum::kVeryPoor; break;
            default: aq = AirQuality::AirQualityEnum::kUnknown; break;
        }
        chip::DeviceLayer::PlatformMgr().LockChipStack();
        g_airQualityInstance->UpdateAirQuality(aq);
        chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    }
}

// ===================================================================
// M702 SENSOR (17-byte frames: 3C 02 ... checksum)
// ===================================================================

static void m702Init()
{
    uart_config_t cfg = {};
    cfg.baud_rate = 9600;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_driver_install(M702_UART, 512, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(M702_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(M702_UART, M702_TX_PIN, M702_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

// Drains the UART; returns true if at least one valid frame was decoded into `out`.
static bool m702Poll(SensorData& out)
{
    static uint8_t frame[17];
    static size_t len = 0;
    bool got = false;

    uint8_t rx[64];
    int n;
    while ((n = uart_read_bytes(M702_UART, rx, sizeof(rx), 0)) > 0) {
        for (int k = 0; k < n; k++) {
            uint8_t b = rx[k];
            // Sync on header 0x3C 0x02
            if (len == 0 && b != 0x3C) continue;
            if (len == 1 && b != 0x02) {
                len = (b == 0x3C) ? 1 : 0;
                continue;
            }
            frame[len++] = b;
            if (len < sizeof(frame)) continue;
            len = 0;

            uint16_t checksum = 0;
            for (int i = 0; i < 16; i++) checksum += frame[i];
            if ((checksum & 0xFF) != frame[16]) {
                ESP_LOGW(TAG, "M702 checksum error");
                continue;
            }

            out.co2  = (frame[2] << 8) | frame[3];
            out.hcho = (frame[4] << 8) | frame[5];
            out.tvoc = (frame[6] << 8) | frame[7];
            out.pm25 = (frame[8] << 8) | frame[9];
            out.pm10 = (frame[10] << 8) | frame[11];
            out.temp = (frame[12] & 0x7F) + (frame[13] / 100.0f);
            if (frame[12] & 0x80) out.temp = -out.temp;
            out.hum  = frame[14] + (frame[15] / 100.0f);
            got = true;
        }
    }
    return got;
}

// ===================================================================
// PAIRING CODE
// ===================================================================

// chip-factory/pin-code, added to the fctry partition by factory/gen_factory.sh
static bool readFactoryPasscode(uint32_t& passcode)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(CONFIG_CHIP_FACTORY_NAMESPACE_PARTITION_LABEL, "chip-factory", NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_u32(h, "pin-code", &passcode);
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No pin-code in the fctry partition (%s): re-run factory/gen_factory.sh", esp_err_to_name(err));
        return false;
    }
    return true;
}

// ===================================================================
// MAIN
// ===================================================================

extern "C" void app_main()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // 1. Display first (splash screen)
    display.init();
    display.setRotation(3);  // Portrait, flipped 180° for the case
    uiShowSplash(display, "Init...");

    // 2. Peripherals
    m702Init();
    gpio_config_t btn = {};
    btn.pin_bit_mask = 1ULL << BUTTON_PIN;
    btn.mode = GPIO_MODE_INPUT;
    btn.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&btn);

    // 3. Matter. The Thread network is owned by Matter:
    //    - not commissioned: advertises over BLE, the Home app sends the
    //      Thread dataset of its border router during pairing and Matter
    //      stores it in NVS
    //    - commissioned: rejoins the stored Thread network on boot
    createMatterNode();

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    // Native 802.15.4 radio, no external RCP/host
    static esp_openthread_platform_config_t ot_config = {};
    ot_config.radio_config.radio_mode = RADIO_MODE_NATIVE;
    ot_config.host_config.host_connection_mode = HOST_CONNECTION_MODE_NONE;
    ot_config.port_config.storage_partition_name = "nvs";
    ot_config.port_config.netif_queue_size = 10;
    ot_config.port_config.task_queue_size = 10;
    set_openthread_platform_config(&ot_config);
#endif

    ESP_ERROR_CHECK(esp_matter::start(app_event_cb));
    initAirQualityInstance();

    s_commissioned = chip::Server::GetInstance().GetFabricTable().FabricCount() > 0;
    if (!s_commissioned) {
        ESP_LOGI(TAG, "Matter node is not commissioned yet.");
        auto flags = chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE);
        char qr[128] = {}, manual[32] = {};
        chip::MutableCharSpan qrSpan(qr, sizeof(qr) - 1), manualSpan(manual, sizeof(manual) - 1);

        // The factory provider only exposes the verifier, so the SDK would fall
        // back to the example passcode: use this device's own from the fctry partition.
        uint32_t passcode = 0;
        bool havePasscode = readFactoryPasscode(passcode);

        chip::PayloadContents payload;
        chip::DeviceLayer::PlatformMgr().LockChipStack();
        CHIP_ERROR qrErr = GetPayloadContents(payload, flags);
        if (qrErr == CHIP_NO_ERROR) {
            payload.setUpPINCode = passcode;
            PrintOnboardingCodes(payload);
            qrErr = GetQRCode(qrSpan, payload);
        }
        CHIP_ERROR codeErr = (qrErr == CHIP_NO_ERROR) ? GetManualPairingCode(manualSpan, payload) : qrErr;
        chip::DeviceLayer::PlatformMgr().UnlockChipStack();

        if (havePasscode && qrErr == CHIP_NO_ERROR && codeErr == CHIP_NO_ERROR) {
            qr[qrSpan.size()] = '\0';
            manual[manualSpan.size()] = '\0';
            // Show as XXXX-XXX-XXXX, like the printed label
            char pretty[40];
            if (strlen(manual) == 11) {
                snprintf(pretty, sizeof(pretty), "%.4s-%.3s-%.4s", manual, manual + 4, manual + 7);
            } else {
                snprintf(pretty, sizeof(pretty), "%s", manual);
            }
            uiShowPairing(display, qr, pretty);
        } else {
            ESP_LOGE(TAG, "Failed to get onboarding codes (is the fctry partition flashed?)");
            uiShowSplash(display, "No pairing data");
        }
    }

    // 4. Main loop
    SensorData latest = {};
    bool newData = false;
    uint32_t previousMillis = 0;
    int updateCounter = 0;
    bool buttonPressed = false;
    uint32_t buttonTimestamp = 0;
    uint32_t resetRequestedAt = 0;

    while (true) {
        uint32_t now = millis();

        // Drain & parse serial (runs continuously)
        if (m702Poll(latest)) newData = true;

        // Timed Matter & display updates
        if (now - previousMillis >= INTERVAL_MS) {
            previousMillis = now;
            if (s_redrawRequested.exchange(false)) {
                // Just paired: leave the QR screen with a full refresh
                if (newData) uiUpdateFull(display, latest);
                else uiShowSplash(display, "Paired!");
                updateCounter = 0;
            }
            if (newData) {
                updateMatter(latest);

                updateCounter++;
                if (!s_commissioned) {
                    // Keep the QR code on screen until paired
                } else if (updateCounter >= FULL_REFRESH_INTERVAL) {
                    uiUpdateFull(display, latest);
                    updateCounter = 0;
                } else {
                    uiUpdatePartial(display, latest);
                }
                newData = false;
            }
        }

        // Button decommissioning (hold BOOT for 5 s)
        bool down = gpio_get_level(BUTTON_PIN) == 0;
        if (down && !buttonPressed) {
            buttonTimestamp = millis();
            buttonPressed = true;
        } else if (!down && buttonPressed) {
            buttonPressed = false;
        }
        if (buttonPressed && (millis() - buttonTimestamp > DECOMMISSIONING_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "Decommissioning Air Sensor Matter Accessory...");
            esp_matter::factory_reset();  // erases NVS and reboots
            buttonTimestamp = millis();
        }

        // Deferred factory reset after the last controller removed the device
        if (s_resetRequested) {
            if (resetRequestedAt == 0) resetRequestedAt = millis() | 1;
            else if (millis() - resetRequestedAt > 2000) esp_matter::factory_reset();
        }

        vTaskDelay(pdMS_TO_TICKS(10));  // yield to the Matter task
    }
}
