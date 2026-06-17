# Project3-MQTT-SmokeTest

## Purpose

ESP32 + DS18B20 MQTT smoke-test node for proving the broker path end to end.

## Device-Specific Scope

- Sensor type: DS18B20
- Sensor count: 1
- Sensor mapping: `probe_1`
- Board profile: ESP32 DevKit
- Primary use: broker connectivity and payload smoke testing
- Supported control topics: none in this minimal build

## Hardware

- Board: ESP32 DevKit (`esp32dev`)
- Sensor: DS18B20 on a 1-wire bus
- Suggested data pin: GPIO4
- Pull-up: 4.7k resistor from data line to 3.3V
- Framework: Arduino

## MQTT Contract Notes

- Telemetry topic: `bbh/iot/{device_code}/telemetry`
- Status topic: `bbh/iot/{device_code}/status`
- State topic: `bbh/iot/{device_code}/state`
- Unit for DS18B20 should be `C`

## Intentional Deviation From Full Baseline

This project is a smoke test, not the full workspace reference.

It keeps the firmware small on purpose and does not yet implement:

- captive portal onboarding
- persisted runtime config
- local diagnostics dashboard
- command / config subscriptions

Use `Project1-ColdRoom-TempMonitor` as the full baseline reference.

## Current Status

MQTT publisher starter is in place. Update Wi-Fi, broker, and `device_code` constants before flashing.
