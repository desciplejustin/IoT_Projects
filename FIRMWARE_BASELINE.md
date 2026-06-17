# Firmware Baseline Contract

> ⚠️ **Wire-format fields in this document are superseded by [`CONTRACT.md`](CONTRACT.md).**
> Neither the firmware nor the backend implements the `sensor_code` / `reading_type`
> reading fields or the separate retained `state` topic described below — the running
> contract uses **`sensor_key`** with no `reading_type`, and computes availability
> server-side (no `state` topic). `CONTRACT.md` is authoritative; the operational and
> security guidance here (onboarding, factory reset, LED behavior, reconnect rules) is
> still current.

Reference implementation: `Project1-ColdRoom-TempMonitor`

Use this baseline for all future telemetry firmware unless a project explicitly documents a deviation in `PROJECT.md`.

## Core Baseline vs Device Scope

This repo uses a two-layer model:

- `FIRMWARE_BASELINE.md` defines the shared device core that all telemetry nodes should inherit.
- Each project's `PROJECT.md` defines the device-specific scope for that node.

The shared core should stay stable across devices.

The device-specific scope should document:

- sensor inventory and sensor types
- board and wiring details
- pin assignments
- publish intervals
- supported command / config topics
- payload mapping choices
- calibration or scaling rules
- any approved baseline deviation

## Primary Transport Contract

Devices publish telemetry with MQTT.

The firmware must treat the MQTT broker as the only upstream transport for normal telemetry and status traffic. HTTP may still be used locally on the device for diagnostics pages, but not as the primary cloud ingestion path.

## Broker and Identity Contract

Each production firmware build must support persisted runtime configuration for:

- `mqtt_host`
- `mqtt_port`
- `mqtt_username`
- `mqtt_password`
- `device_code`
- `topic_root`
- telemetry interval
- status interval

Recommended defaults:

- topic root: `bbh/iot`
- MQTT port: `1883` on trusted LANs, `8883` when TLS is enabled

Stored configuration must survive reboot using NVS / Preferences or the platform-equivalent persistent store.

## Topic Contract

Every device owns a topic namespace:

- `{topic_root}/{device_code}/telemetry`
- `{topic_root}/{device_code}/status`
- `{topic_root}/{device_code}/state`
- `{topic_root}/{device_code}/command`
- `{topic_root}/{device_code}/config`
- `{topic_root}/{device_code}/ota`

Topic rules:

- `telemetry` is for sensor readings only
- `status` is for JSON heartbeat / health snapshots
- `state` is for retained availability state such as `online` and `offline`
- `command`, `config`, and `ota` are reserved for backend-to-device control paths

MQTT behavior rules:

- publish retained LWT `offline` to `state`
- publish retained `online` to `state` after a successful MQTT session is established
- publish telemetry with broker acknowledgment enabled where the MQTT client supports QoS 1
- do not retain telemetry messages
- do not publish to shared wildcard topics from the device

## Telemetry Payload Contract

Every telemetry message must include:

- `schema_version`
- `device_code`
- `message_id` (unique per device message)
- `firmware_version`
- `uptime_seconds`
- `readings` array

Every reading entry must include (see [`CONTRACT.md`](CONTRACT.md) — implemented form):

- `sensor_key`  (NOT `sensor_code`)
- `value`
- `unit`

`reading_type` is not sent in telemetry; the backend derives it from the registered
sensor row.

Recommended optional fields:

- `recorded_at` when the device has reliable clock sync
- `wifi_rssi`
- `ip_address`

Sensor and unit rules:

- sensor codes must be deterministic (`probe_1`, `probe_2`, `moisture_1`, etc.)
- units must be canonical backend units only: `C`, `%`, `pH`, `mS/cm`
- the topic device code and payload device code must match

If a device does not have reliable time, the backend should stamp receive time and treat `recorded_at` as optional.

## Status Payload Contract

Every status message should include:

- `device_code`
- `firmware_version`
- `uptime_seconds`
- `wifi_rssi`
- `ip_address`
- `status`

Recommended status values:

- `online`
- `degraded`
- `offline`
- `setup`

## Operational UX Contract

- Captive portal onboarding must be available for first-time setup and Wi-Fi recovery.
- Local diagnostics server must run on port `80`.
- Local diagnostics endpoints:
  - `/` (HTML dashboard)
  - `/api/status` (JSON)
- Local dashboard auth is required (Basic Auth or equivalent).

LED behavior contract:

- red blink while connecting
- blue when Wi-Fi and MQTT are both healthy
- red solid on failure timeout

Factory reset contract:

- long-press reset must be non-blocking
- must clear Wi-Fi credentials and stored MQTT/device configuration

## Runtime Behavior Contract

- telemetry publish interval must be configurable and documented per project
- status heartbeat interval must be configurable and documented per project
- firmware must reconnect after Wi-Fi loss
- firmware must reconnect after MQTT disconnect
- firmware must resubscribe to control topics after reconnect
- firmware must avoid blocking the main loop for long retry windows

## Security Contract

- do not commit production credentials into source control
- prefer per-device or per-device-class MQTT credentials
- restrict broker access by topic where practical
- move production deployments to TLS when remote or untrusted networks are involved

## Production-Ready Verification Checklist

- Device onboards via captive portal and reconnects after Wi-Fi loss.
- Persisted MQTT and device settings survive reboot.
- Device publishes retained `online` / `offline` state correctly.
- Telemetry arrives on the expected topic and matches the documented payload schema.
- Local auth-protected status UI is reachable and reflects live state.
- Sensor mapping is stable and deterministic.
