# PC Monitor & WOL (StickC Plus2)

Real-time PC monitoring and remote Wake-on-LAN firmware for **M5StickC Plus2**.

## Features

- Real-time PC system monitoring:
  - CPU usage
  - RAM usage
  - GPU usage and temperature
  - Network upload/download speed
- Remote Wake-on-LAN (WOL)
- Stateless MQTT-based design
- Automatic reconnect and always-on operation
- On-device display with burn-in protection
- Simple reset and configuration via buttons

## Hardware Requirements

- M5StickC Plus2

## First-Time Setup (Required)

After flashing the firmware:

1. Power on the M5StickC Plus2  
2. Connect to the WiFi network **`M5-Setup`** using a phone or PC  
3. A configuration portal will open automatically  
4. Select and connect the device to your own WiFi network  
5. Save and reboot

After setup, the device will automatically reconnect to WiFi on every boot.

## Software Requirements

### Windows PC Status Sender Tool (Required)

To display PC status data, a small Windows sender tool must be installed on the PC.

Download and setup instructions:  
https://kreaxv.top/m5setup/

### Web Wake-on-LAN

Wake-on-LAN commands can also be sent directly from the web:

https://wol.kreaxv.top/

## How It Works

- The Windows PC sender tool collects system metrics and sends them over the network
- The M5StickC Plus2 displays real-time PC status on its screen
- Wake-on-LAN packets can be triggered remotely via:
  - Web interface
  - MQTT messages
- The device uses a stateless design and does not store per-device credentials

## Firmware Distribution

- This repository contains the **source code**
- Precompiled firmware binaries may be distributed separately
- Users can build the firmware themselves if verification is required

## Build Environment

- PlatformIO
- Arduino framework
- ESP32 (M5StickC Plus2)

## License

MIT License
