# sensorMonitor_v1 — ESP-IDF / esp-matter build

Native ESP-IDF port of `../sensorMonitor_v1.ino`. Same hardware, pins, Matter endpoints
(1 = temperature, 2 = humidity, 3 = air quality + CO2/HCHO/TVOC/PM2.5/PM10), and screen layout.

| File | Replaces |
|---|---|
| `main/app_main.cpp` | `setup()`/`loop()`, Arduino `Matter`, `HardwareSerial`, `WiFi` |
| `main/epd.cpp/.h` | GxEPD2 (`GxEPD2_420_GDEY042T81` + `GxEPD2_BW`) + Adafruit GFX text |
| `main/ui.cpp/.h` | display cards, IAQI, change detection |
| `main/fonts/` | Adafruit GFX fonts (unchanged) |

esp-matter is pulled from the ESP component registry (`main/idf_component.yml`), so you only
need ESP-IDF installed (v5.5.x recommended, which matches Arduino core 3.3.11).

## Build & flash

```sh
. $IDF_PATH/export.sh          # or use the VS Code ESP-IDF extension
cd idf
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyACM0 erase-flash flash monitor
```

`erase-flash` is recommended on the first flash, so the old Arduino commissioning data in NVS
doesn't conflict. Afterwards, re-pair the device in your Home app. The pairing code and QR URL
are printed on the serial monitor. Hold BOOT for 5 s to decommission.
