# Project1-ColdRoom-TempMonitor

## Purpose

Reference BBH MQTT bootstrap firmware for a cold-room monitoring node using one ESP32 with two DS18B20 probes on a shared 1-wire bus.

## Device-Specific Scope

- Sensor type: DS18B20 temperature probes
- Sensor count: 2
- Default sensor mapping: `probe_1`, `probe_2`
- Board profile: ESP32 DevKit
- Primary use: cold-room temperature monitoring
- Bootstrap topics:
  - claim publish: `bbh/iot/bootstrap/claim`
  - reply subscribe: `bbh/iot/bootstrap/reply/{hardware_id}`
- Runtime topics after approval:
  - status: `bbh/iot/{device_key}/status`
  - telemetry: `bbh/iot/{device_key}/telemetry`

## Hardware

- Board: ESP32 DevKit (`esp32dev`)
- Sensor bus: DS18B20 x2 on a single 1-wire data line
- Suggested data pin: GPIO4
- Pull-up: 4.7k resistor from data line to 3.3V

## Firmware Behavior

- Uses MQTT-only bootstrap onboarding
- Publishes bootstrap claims with shared bootstrap MQTT credentials
- Waits for pending / approved replies on `bbh/iot/bootstrap/reply/{hardware_id}`
- Stores approved per-device MQTT config locally
- Reconnects with final per-device MQTT credentials after approval
- Publishes runtime status only to `{topic_base}/status`
- Publishes runtime telemetry only to `{topic_base}/telemetry`
- Sends telemetry every 30 seconds
- Uses MQTT status payloads for online / offline state and does not publish a separate `state` topic

## Persisted Runtime Settings

- bootstrap broker host / port
- bootstrap MQTT username / password
- bootstrap MQTT client ID
- bootstrap claim topic
- bootstrap reply prefix
- location hint
- final device key
- final MQTT username / password
- final MQTT client ID
- final runtime topic base
- final runtime status / telemetry topics
- final QoS / keepalive
- expected interval / offline settings
- sensor definitions returned by the backend

## Current Default Bootstrap Settings

- Bootstrap broker host: `10.24.16.94`
- Bootstrap broker port: `1883`
- Bootstrap claim topic: `bbh/iot/bootstrap/claim`
- Bootstrap reply prefix: `bbh/iot/bootstrap/reply`
- Bootstrap client ID: `bbh-iot-bootstrap`
- Location hint: `Coldroom North`

Important:

- Set the shared bootstrap MQTT credentials through the captive portal or a local `platformio_override.ini`.
- Copy `platformio_override.example.ini` to `platformio_override.ini` for local build-time bootstrap defaults that should not be committed.
- Final runtime MQTT credentials come only from the approved bootstrap response.

## Current Status

This is the workspace reference implementation for BBH MQTT bootstrap onboarding.

## Easiest Way To Run It

If you are using VS Code with PlatformIO:

1. Open the Command Palette.
2. Run `Tasks: Run Task`.
3. Choose one of these:
   - `PIO: Upload Project1 Scan` for the first sensor-ID test.
   - `PIO: Upload Project1 App` for the normal MQTT bootstrap firmware.

If you prefer the terminal, use:

```bash
pio run -d Project1-ColdRoom-TempMonitor -e esp32dev_scan -t upload
pio device monitor -d Project1-ColdRoom-TempMonitor -b 115200
```

or:

```bash
pio run -d Project1-ColdRoom-TempMonitor -e esp32dev_app -t upload
pio device monitor -d Project1-ColdRoom-TempMonitor -b 115200
```

## First Hardware Test

1. Wire both DS18B20 sensors on one 1-wire bus to GPIO4.
2. Ensure a 4.7k pull-up resistor from data line to 3.3V.
3. Build and upload scan mode with `esp32dev_scan`.
4. Open serial monitor at 115200.
5. Record both ROM IDs printed as `Sensor N ROM: ...`.

If two IDs are printed and temperatures update every few seconds, the board and sensor bus are working.
