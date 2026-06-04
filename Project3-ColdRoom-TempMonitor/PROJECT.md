# Project3-ColdRoom-TempMonitor

## Purpose

Receiving cold-room temperature measurements from one ESP32 with two DS18B20 probes on a shared 1-wire bus and send readings to the IoT backend.

## Hardware

- Board: ESP32 DevKit (`esp32dev`)
- Sensor bus: DS18B20 x2 on single 1-wire data line
- Suggested data pin: GPIO4
- Pull-up: 4.7k resistor from data line to 3.3V

## Firmware Behavior

- Reads two sensors (`probe_1`, `probe_2`) every 30 seconds.
- Posts readings to `/api/iot/v1/readings` with `x-device-token` header.
- Sends unit `C` for both probes.

## Backend Setup Requirements

- Device key should match firmware: `ESP32-COLDROOM-01` (or update firmware)
- Register two sensor keys for this device: `probe_1`, `probe_2`
- Unit for both sensors should be `C`

## Current Status

Project scaffolded and ready for Wi-Fi/API credential updates and first flash.

## Easiest Way To Run It

If you are using VS Code with PlatformIO, the quickest path is:

1. Open the Command Palette.
2. Run `Tasks: Run Task`.
3. Choose one of these:
   - `PIO: Flash & Monitor Scan` for the first sensor-ID test.
   - `PIO: Flash & Monitor App` for the normal temperature posting firmware.

If you prefer the terminal, use:

```bash
pio run -e esp32dev_scan -t upload
pio device monitor -e esp32dev_scan
```

or:

```bash
pio run -e esp32dev_app -t upload
pio device monitor -e esp32dev_app
```

## First Hardware Test (Board + Sensor ID Scan)

1. Wire both DS18B20 sensors on one 1-wire bus to GPIO4.
2. Ensure a 4.7k pull-up resistor from data line to 3.3V.
3. Build and upload scan mode with `esp32dev_scan`.
4. Open serial monitor at 115200.
5. Record both ROM IDs printed as `Sensor N ROM: ...`.

If two IDs are printed and temperatures update every few seconds, board and sensor bus are working.
