# IoT Firmware Workspace

This repository is the shared workspace for all firmware built under `IoT_Projects`.

It is organized as a multi-project PlatformIO repo, where each device or firmware line has its own top-level folder and its own `platformio.ini`.

## Layout

- `Project1-ColdRoom-TempMonitor/` - MQTT baseline reference for a 2x DS18B20 cold-room node
- `Project2/` - ESP8266 smoke-test sandbox
- `Project3-MQTT-SmokeTest/` - ESP32 MQTT smoke test with one DS18B20
- `FIRMWARE_BASELINE.md` - mandatory repo-wide firmware contract
- `iot_mqtt_integration_plan.md` - system architecture and rollout plan
- `projects.registry.json` - machine-readable project catalog
- `tools/new-pio-project.ps1` - project generator
- `.vscode/tasks.json` - one-click build/upload/monitor tasks

Each project folder must contain:

- `platformio.ini`
- `src/main.cpp`
- `PROJECT.md`

## Recommended Workspace Pattern

- Treat the repo root as a workspace container, not a deployable firmware app.
- Keep one board / firmware purpose per project folder unless a project has a documented reason for multiple environments.
- Use `Project1-ColdRoom-TempMonitor` as the MQTT-first reference implementation.

## Core Baseline

All new telemetry nodes should follow the repo baseline in `FIRMWARE_BASELINE.md`.

Shared device rules:

1. Publish telemetry and heartbeat data with MQTT.
2. Keep local HTTP only for on-device diagnostics and setup UX.
3. Use stable `device_code` and deterministic `sensor_code` values.
4. Persist Wi-Fi and MQTT configuration locally.
5. Publish device availability with retained `state` messages.

## Device-Specific Scope

Each project then defines its own scope in `PROJECT.md`, including:

- sensor types and quantity
- pinout and wiring notes
- sampling / publish interval
- payload mapping for that device
- command / config topics it supports
- any intentional deviation from the common baseline

This gives you one shared firmware contract plus one device-specific overlay per project.

## Fast Workflow

1. Open this repo in VS Code.
2. Use `Terminal -> Run Task`.
3. Run one of these tasks:
   - `PIO: Build Project1 App`
   - `PIO: Upload Project1 App`
   - `PIO: Build Project1 Scan`
   - `PIO: Upload Project1 Scan`
   - `PIO: Monitor Project1`
   - `PIO: Build Project2`
   - `PIO: Upload Project2`
   - `PIO: Monitor Project2`
   - `PIO: Build Project3`
   - `PIO: Upload Project3`
   - `PIO: Monitor Project3`

## Create New Project

Use PowerShell from repository root:

```powershell
.\tools\new-pio-project.ps1 -Name Project04-ESP32-Greenhouse -Board esp32dev -Platform espressif32 -Description "ESP32 greenhouse telemetry node"
```

The generator creates a clean project folder and updates `projects.registry.json`.

## Naming Guidance

- Folder names: `Project04-ESP32-Greenhouse`, `Project05-ESP32-PumpNode`
- Device codes: `greenhouse-01`, `coldroom-temp-001`
- Sensor codes: `probe_1`, `probe_2`, `ambient_humidity`, `moisture_1`

## Current Reference Split

- `Project1-ColdRoom-TempMonitor` is the best-practice reference for future telemetry firmware in this repo.
- `Project2` is a basic non-telemetry board test project.
- `Project3-MQTT-SmokeTest` is a minimal MQTT publisher for quick broker-path testing.
