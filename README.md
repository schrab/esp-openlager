# ESP-OpenLager (ESP32-C3 Port)

High-speed UART logging to microSD card for ESP32-C3.

This is a port of the original [OpenLager](https://github.com/d-ronin/openlager) firmware to the ESP32-C3, enabling high-speed logging (up to 2Mbps) for Flight Controllers (Betaflight, etc.).

## Features
- **High Speed**: Supports UART baud rates up to 2Mbps.
- **Auto-Naming**: Automatically names log files based on the "Craft Name" sent in the Blackbox header.
- **Session Management**: Automatically starts new log files when arming/disarming is detected (based on data flow).
- **Heartbeat LED**: Slow heartbeat blink when disarmed, fast activity flicker when logging.

## Pinout (ESP32-C3 SuperMini)
- **UART RX**: GPIO 0
- **SD CS**: GPIO 1
- **SD MOSI**: GPIO 2
- **SD CLK**: GPIO 3
- **SD MISO**: GPIO 4
- **LED**: GPIO 8

## Getting Started

### Prerequisites
- ESP-IDF v5.x

### Build & Flash
```bash
idf.py set-target esp32c3
idf.py build
idf.py -p [PORT] flash monitor
```

## License
Simplified BSD license. See `main/main.c` for details.
