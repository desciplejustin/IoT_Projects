# Project1

## Purpose

ESP32 + DS18B20 node that posts readings to the IoT backend contract.

## Hardware

- Board: ESP32 DevKit (`esp32dev`)
- Sensor: DS18B20 (OneWire)
- Framework: Arduino

## Firmware Contract Notes

- Endpoint: `/api/iot/v1/readings`
- Header required: `x-device-token`
- Payload includes `device_key`, `message_id`, `reading_time`, `readings[]`
- Unit for DS18B20 should be `C`

## Current Status

Starter implementation in place. Update Wi-Fi credentials, API URL, device key, and token before flashing.
