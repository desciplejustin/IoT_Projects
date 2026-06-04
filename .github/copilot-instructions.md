# IoT Workspace Rules

These rules are mandatory for all future IoT firmware projects in this repository.

## Project Structure

- Each firmware project must have its own top-level folder.
- Each project folder must include:
  - `platformio.ini`
  - `src/main.cpp`
  - `PROJECT.md`
- Do not mix multiple board targets in one project folder unless there is a clear reason.

## Metadata and Documentation

- Every new project must be added to `projects.registry.json`.
- `PROJECT.md` must include:
  - Purpose
  - Board/platform/framework
  - Sensor/wiring notes
  - Current status

## Naming and Contract

- Project folders should use descriptive names, for example `Project03-ESP32-ColdRoom`.
- Device keys should be stable and explicit, for example `ESP32-COLDROOM-01`.
- Sensor keys should be deterministic, for example `probe_1`, `probe_2`.
- Use only canonical units expected by backend: `C`, `%`, `pH`, `mS/cm`.

## Workflow

- New projects should be generated with `tools/new-pio-project.ps1` when possible.
- Add VS Code tasks in `.vscode/tasks.json` for build, upload, and monitor of each project.
- Keep credentials out of committed code where possible; use build flags or ignored local config.

## Firmware Baseline Contract (Mandatory)

Use `Project3-ColdRoom-TempMonitor` as the protocol baseline for all future telemetry nodes unless a project explicitly documents a deviation in its `PROJECT.md`.

- Backend ingestion endpoint path must remain `/api/iot/v1/readings` (current backend host/port uses port `5010`).
- Device claim flow must use `/api/iot/v1/device-claims` create + poll lifecycle and handle `pending`, `approved`, `expired`, `rejected`, and `claim_not_found` recovery.
- Telemetry requests must include headers: `Content-Type: application/json`, `Accept: application/json`, and `x-device-token`.
- Telemetry body must include `device_key`, unique `message_id`, and `readings[]` entries with `sensor_key`, `value`, and canonical `unit`.
- Sensor keys must be deterministic (`probe_1`, `probe_2` pattern for probes) and units must be canonical (`C`, `%`, `pH`, `mS/cm`).
- Device should persist approved credentials and location metadata in NVS/Preferences and restore at boot.
- Local diagnostics server on port `80` with Basic Auth is required for status visibility (`/` and `/api/status`).
- LED behavior contract: red blink while connecting, blue when connected/success, red solid on failure timeout.
- Factory reset long-press must be non-blocking and clear Wi-Fi plus stored device credentials.
- Default sampling and send interval should be `30s` unless backend returns `expected_interval_minutes`.

## Verification Contract

Before calling a firmware build production-ready, validate all of the following:

- Device can onboard via captive portal and recover from Wi-Fi loss.
- Claim approval stores credentials and survives reboot.
- Telemetry returns HTTP success and backend `saved_count` updates.
- Local status UI loads with auth and shows live probe values plus recent event logs.
- Both probes are read from one-wire bus and mapped to stable sensor keys.
