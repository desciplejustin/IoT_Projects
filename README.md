# IoT Multi-Project Workspace

This repository is organized for fast iteration across many devices/boards.

## Layout

- `Project1/` - ESP32 + DS18B20 + IoT API test
- `Project2/` - ESP8266 smoke test
- `Project3-ColdRoom-TempMonitor/` - ESP32 + 2x DS18B20 cold room monitor
- `projects.registry.json` - machine-readable project catalog
- `tools/new-pio-project.ps1` - project generator
- `.vscode/tasks.json` - one-click build/upload/monitor tasks
- `.github/copilot-instructions.md` - repository rules for future projects

Each project folder contains:

- `platformio.ini`
- `src/main.cpp`
- `PROJECT.md` (purpose, board, status, notes)

## Fast Workflow

1. Open this repo in VS Code.
2. Use `Terminal -> Run Task`.
3. Run one of these tasks:
   - `PIO: Build Project1`
   - `PIO: Upload Project1`
   - `PIO: Monitor Project1`
   - `PIO: Build Project2`
   - `PIO: Upload Project2`
   - `PIO: Monitor Project2`
   - `PIO: Build Project3-ColdRoom-TempMonitor`
   - `PIO: Upload Project3-ColdRoom-TempMonitor`
   - `PIO: Monitor Project3-ColdRoom-TempMonitor`

## Create New Project (Clean-Slate Pattern)

Use PowerShell from repository root:

```powershell
.\tools\new-pio-project.ps1 -Name Project3 -Board esp32dev -Platform espressif32 -Description "ESP32 greenhouse node"
```

This creates a complete PlatformIO project and updates `projects.registry.json`.

## Recommended Naming

- Folder names: `Project01-ESP32-Greenhouse`, `Project02-ESP32-PumpNode`
- Device key format: `ESP32-HS6-01`, `ESP32-HS6-02`
- Sensor key format: `probe_1`, `probe_2`, `ambient_humidity`

## Quick Test Checklist

1. Register device in IoT UI and save token.
2. Register all sensor keys and valid units (`C`, `%`, `pH`, `mS/cm`).
3. Flash firmware.
4. Verify API response contains `ok: true` and `saved_count`.
5. Confirm dashboard `last_seen` and latest sensor values update.

## Firmware Protocol Baseline (All New Builds)

All future IoT firmware projects should follow the same working protocol used by `Project3-ColdRoom-TempMonitor`.

- Primary spec: `FIRMWARE_BASELINE.md`
- Enforced repo rules: `.github/copilot-instructions.md`

High-level baseline:

1. Use claim onboarding (`/api/iot/v1/device-claims`) and persist approved credentials.
2. Send telemetry to `/api/iot/v1/readings` with `x-device-token` and unique `message_id`.
3. Use deterministic sensor keys and canonical units.
4. Expose local authenticated diagnostics UI (`/` and `/api/status`).
5. Implement LED and factory-reset behavior consistent with the cold-room reference firmware.
