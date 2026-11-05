# Air Quality Sensor V1

## Description

This is a simple device that uses an ESP32, an e-ink screen and a sensor to monitor indoor air quality, show it on the display and broadcast it over the Matter protocol to every platform that supports it, i.e. HomeKit, Home Assistant, Google Home, etc.

### Components

- [Waveshare ESP32-C6-Zero](https://docs.waveshare.com/ESP32-C6-Zero?variant=ESP32-C6-Zero), but I suppose you could use any ESP32-C6 or Matter-compatible board with some tiny adjustments.
- [WeAct 4.2"](https://github.com/WeActStudio/WeActStudio.EpaperModule) e-paper module. I got the big b/w one; again, you can change it easily.
- M702 UART sensor. I got this one because I liked its quality and the things it measures.
- 3D-printed case. I have added the 3MF file to the repo if you want it.

### Wiring

#### E-ink screen (WeAct 4.2")

| Screen pin       | ESP32-C6-Zero |
| ---------------- | ------------- |
| VCC              | 3V3           |
| GND              | GND           |
| SDA (DIN / MOSI) | GP21          |
| SCL (CLK / SCK)  | GP22          |
| CS               | GP20          |
| D/C              | GP19          |
| RES              | GP18          |
| BUSY             | GP14          |

#### M702 sensor

| Sensor pin | ESP32-C6-Zero |
| ---------- | ------------- |
| VCC        | 5V            |
| GND        | GND           |
| TX         | GP0           |
| NC         | —             |

> **Note:** The sensor needs 5V and the screen 3.3V BE CAREFUL!

### Code

I used the Arduino IDE because the Espressif Matter SDK is a pain to compile and work with, and I am not a big fan of C++ either. Maybe in the future I will port the code to the SDK so I can use Thread instead of Wi-Fi, but who knows.

I formatted and commented the code a lot. For some of the sensors, the Arduino Matter library still doesn't have classes, so in those cases I imported them from the Matter libraries and had to instantiate everything manually.

> this is a WIP I will add more documentation, images and details later.
