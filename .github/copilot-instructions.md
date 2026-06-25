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
  - purpose
  - board / platform / framework
  - sensor / wiring notes
  - MQTT topic usage
  - device-specific scope
  - current status

## Naming and Contract

- Project folders should use descriptive names, for example `Project03-ESP32-ColdRoom`.
- Device codes should be stable and explicit, for example `coldroom-temp-001`.
- Sensor codes should be deterministic, for example `probe_1`, `probe_2`.
- Use only canonical backend units expected by storage and dashboards: `C`, `%`, `pH`, `mS/cm`.

## Workflow

- New projects should be generated with `tools/new-pio-project.ps1` when possible.
- Add VS Code tasks in `.vscode/tasks.json` for build, upload, and monitor of each project.
- Keep credentials out of committed code where possible; use runtime setup or ignored local config.

## Firmware Baseline Contract (Mandatory)

Use `Project1-ColdRoom-TempMonitor` as the MQTT-first baseline for all future telemetry nodes unless a project explicitly documents a deviation in its `PROJECT.md`.

- Devices must publish telemetry through MQTT, not HTTP, for normal upstream ingestion.
- Required persisted runtime settings: `mqtt_host`, `mqtt_port`, `mqtt_username`, `mqtt_password`, `device_code`, `topic_root`, and publish intervals.
- Device topic namespace must use:
  - `{topic_root}/{device_code}/telemetry`
  - `{topic_root}/{device_code}/status`
  - `{topic_root}/{device_code}/state`
  - `{topic_root}/{device_code}/command`
  - `{topic_root}/{device_code}/config`
- Telemetry payloads must include `schema_version`, `device_code`, unique `message_id`, `firmware_version`, and `readings[]`.
- Each reading must include `sensor_code`, `reading_type`, `value`, and canonical `unit`.
- Devices should publish retained availability state (`online` / `offline`) and reconnect cleanly after Wi-Fi or broker loss.
- Local diagnostics server on port `80` with auth is required for status visibility (`/` and `/api/status`) on baseline telemetry nodes.
- LED behavior contract: red blink while connecting, blue when Wi-Fi and MQTT are healthy, red solid on failure timeout.
- Factory reset long-press must be non-blocking and clear Wi-Fi plus stored MQTT/device configuration.

## Device-Specific Scope (Mandatory)

Each telemetry project must also document its own scope in `PROJECT.md`, including:

- sensor inventory and count
- wiring and pins
- publish interval and heartbeat interval
- supported config / command topics
- calibration or transformation rules
- any baseline deviations

## Verification Contract

Before calling a firmware build production-ready, validate all of the following:

- Device can onboard via captive portal and recover from Wi-Fi loss.
- Persisted MQTT and device settings survive reboot.
- Retained availability state updates correctly on broker reconnect.
- Telemetry arrives on the expected topic and matches the baseline payload schema.
- Local status UI loads with auth and shows live sensor values plus recent event logs.
