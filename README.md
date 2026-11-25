# ESP32 Classroom Noise Monitoring System

An embedded-systems project that monitors classroom sound levels with an ESP32 and sends telemetry to ThingsBoard. The firmware also receives a remote command to activate a buzzer.

## Features visible in the firmware
- Reads an analog microphone sensor
- Calculates a noise reading and alert state
- Uses FreeRTOS tasks and synchronization primitives
- Publishes telemetry to ThingsBoard over MQTT
- Receives an MQTT RPC command to trigger the buzzer
- Controls an LED and buzzer using ESP32 GPIO pins

## Hardware and software
- ESP32 development board
- Analog microphone / sound sensor
- LED and buzzer
- Arduino IDE or compatible Arduino toolchain
- Wi-Fi network and ThingsBoard device/dashboard

Check pin definitions in `Noise_Detect.ino` and wire components to match the configured GPIO pins.

## Setup
1. Install ESP32 board support in Arduino IDE.
2. Install the PubSubClient and ArduinoJson libraries.
3. Create a ThingsBoard device and configure its token locally before uploading.
4. Configure Wi-Fi credentials locally.
5. Review GPIO assignments and calibrate the microphone threshold.
6. Upload the sketch and open the serial monitor.
7. Configure a ThingsBoard dashboard for telemetry and the supported RPC command.

## Security warning
**Never commit a real Wi-Fi password or ThingsBoard device token.** The token previously present in this public repository must be considered exposed and should be revoked or regenerated in ThingsBoard. Updating source code does not remove the old token from Git history.

The repository currently contains the ESP32 firmware. Add a wiring diagram, component list, dashboard screenshots, and a demonstration video to make it easier to reproduce.
