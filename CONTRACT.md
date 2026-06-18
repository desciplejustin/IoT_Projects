# IoT Wire Contract (Authoritative)

This document defines the **actual** MQTT wire contract between BBH IoT devices and
the `bbh-app25.03` backend. It is derived from the running code on both sides:

- Firmware: `shared_libs/BBHIotFirmwareCore` (the shared core all telemetry nodes use)
- Backend: `bbh-app25.03/backend/src/services/iot*.ts` and `sql-pg/.../047_iot_schema.sql`

> **Authority:** When this file disagrees with `FIRMWARE_BASELINE.md` or
> `iot_mqtt_integration_plan.md`, **this file wins.** Those two documents describe an
> earlier design (notably `sensor_code` / `reading_type` / a separate `state` topic)
> that neither the firmware nor the backend actually implements. Treat them as
> historical until they are reconciled to match this contract.

---

## Identity

- A device is identified by its **`device_key`**, which is the last path segment of its
  topic base: `{topic_prefix}/{device_key}`.
- `topic_prefix` defaults to `bbh/iot` (backend env `IOT_MQTT_TOPIC_PREFIX`).
- The backend resolves the device by parsing `device_key` out of the topic, **not** from
  the payload. Payload `device_code` / `schema_version` / `uptime_seconds` are currently
  ignored by ingestion — they are allowed but not required.
- A device must already exist (and be active) in `iot_devices` before its telemetry is
  accepted. New devices are registered via the bootstrap/claim flow below.

## Topics

| Topic | Direction | Retained | Notes |
|---|---|---|---|
| `{prefix}/{device_key}/telemetry` | device → backend | no | sensor readings |
| `{prefix}/{device_key}/status`    | device → backend | no | JSON heartbeat |
| `{prefix}/{device_key}/command`   | backend → device | no | actuator commands (see Actuator control) |
| `{prefix}/{device_key}/state`     | device → backend | yes (recommended) | actuator state reports |
| `{prefix}/{device_key}/config`    | backend → device | — | reserved (not yet consumed by firmware) |
| `bbh/iot/bootstrap/claim`         | device → backend | no | unprovisioned device claim |
| `bbh/iot/bootstrap/reply/{hardware_id}` | backend → device | see note | claim status / approved config |

Notes:

- **There is no `ota` topic yet.** Device availability (online/offline) is computed
  server-side from `iot_devices.last_seen` + `offline_after_minutes` — the `state` topic is
  for **actuator** state, not device availability.
- The `command` and `state` topics are consumed by the backend now (for switches/relays),
  but the **device firmware does not yet act on commands or publish state** — that is the
  pending actuator-firmware work. Until then commands publish but go unconfirmed.
- `hardware_id` in the reply topic is the device MAC, sanitized so every run of characters
  outside `[A-Za-z0-9._-]` collapses to a single `-`. Firmware and backend sanitize
  identically, so `AA:BB:CC...` → `AA-BB-CC...` on both ends.
- All publishes use **QoS 1**.

## Telemetry payload (device → `…/telemetry`)

```json
{
  "message_id": "coldroom-temp-001-7-000044",
  "firmware_version": "coldroom-temp-1.0.0",
  "reading_time": "2026-06-18T09:21:04Z",
  "readings": [
    { "sensor_key": "probe_1", "value": 24.6, "unit": "C" },
    { "sensor_key": "probe_2", "value": 25.1, "unit": "C" }
  ]
}
```

Rules:

- The canonical reading field is **`sensor_key`** (NOT `sensor_code`). The backend reads
  `reading.sensor_key`; anything else is quarantined as `missing_sensor_key`.
- `reading_type` is **not** part of the telemetry payload. The backend takes the reading
  type from the registered sensor row (`iot_sensors.sensor_type`), not the message.
- `value` must be a finite number. `unit` must be canonical **and** match the sensor's
  configured unit.
- `message_id` **must be globally unique per (device, sensor) over the lifetime of the
  device**, including across reboots. The backend enforces a unique index on
  `(device_id, sensor_id, ingest_message_id)` and silently drops conflicts via
  `ON CONFLICT DO NOTHING`. The firmware guarantees this with a persisted boot sequence:
  `message_id = {device_key}-{boot_seq}-{counter}` where `boot_seq` is stored in NVS and
  incremented every boot. **Never ship a `message_id` whose counter restarts at boot
  without a per-boot prefix** — it causes silent post-reboot data loss.
- Optional `reading_time` (ISO 8601, UTC) is sent **only** when the device has reliable
  clock sync. The field name is exactly `reading_time` (the planning doc's `recorded_at`
  is wrong). The firmware syncs time via NTP (`pool.ntp.org`) and stamps each record when
  the sample is taken; if NTP has not yet resolved, the field is omitted and the backend
  stamps server receive time.

### Offline buffering (store-and-forward)

The firmware does not drop telemetry while MQTT is unavailable. Each sample is written to a
bounded persistent ring buffer in flash (LittleFS, ~512 records) and flushed oldest-first on
reconnect. Because `message_id` and `reading_time` are assigned when the sample is taken and
stored with the record, buffered readings keep their original identity and timestamp through
the outage and across reboots — so the backend dedups and time-orders them correctly on
flush. When the buffer is full the oldest record is dropped to make room for the newest.

### Canonical units

`C`, `%`, `pH`, `mS/cm`. Anything else is quarantined as `invalid_unit`.

### Backend deduplication

Ingestion dedups on **either** unique index:

- `(device_id, sensor_id, ingest_message_id)` — primary dedup (always populated)
- `(device_id, sensor_id, reading_hash)` — `reading_hash = sha256(device_key, sensor_key,
  reading_time, value, unit)`. Because `reading_time` defaults to server `now()` when the
  device omits it, this index rarely fires today; `message_id` is doing the real work.

## Status payload (device → `…/status`)

What the firmware sends:

```json
{
  "state": "online",
  "firmware_version": "coldroom-temp-1.0.0",
  "ip_address": "10.24.16.41",
  "device_key": "coldroom-temp-001",
  "wifi_rssi": -63,
  "uptime_seconds": 381240
}
```

The backend parses the JSON heartbeat and persists `ip_address`, `firmware_version`,
`wifi_rssi`, and `uptime_seconds` onto `iot_devices` (columns `ip_address`,
`firmware_version`, `last_wifi_rssi`, `last_uptime_seconds`, `last_status_at`), in addition
to refreshing the heartbeat timestamps. Fields are updated with `COALESCE`, so a partial or
plain-text heartbeat never clears previously stored values. `state` is not persisted —
online/offline is still derived server-side from `last_seen` + `offline_after_minutes`.

## Bootstrap / claim flow

1. An unprovisioned device publishes a **claim** to `bbh/iot/bootstrap/claim`:

   ```json
   {
     "hardware_id": "AA:BB:CC:DD:EE:FF",
     "firmware_version": "coldroom-temp-1.0.0",
     "location_hint": "Coldroom North",
     "ip_address": "10.24.16.41",
     "sensors": [
       { "sensor_key": "probe_1", "sensor_type": "temperature", "unit": "C" }
     ]
   }
   ```

   `hardware_id` and at least one valid sensor (canonical unit) are required.

2. The backend replies on `bbh/iot/bootstrap/reply/{hardware_id}` with a **pending** status:

   ```json
   { "kind": "claim_status", "status": "pending", "claim_id": "…", "claim_code": "123456" }
   ```

3. After an operator approves the claim, the backend publishes the **approved config**:

   ```json
   {
     "kind": "device_config",
     "status": "approved",
     "device": { "key": "coldroom-temp-001", "name": "…", "type": "…",
                 "location_code": "…", "expected_interval_minutes": 5,
                 "offline_after_minutes": 15 },
     "mqtt": { "username": "iot-coldroom-temp-001", "password": "…",
               "client_id": "coldroom-temp-001", "topic_base": "bbh/iot/coldroom-temp-001",
               "qos": 1, "keepalive_seconds": 60,
               "broker": { "host": "…", "port": 1883, "protocol": "mqtt", "tls": false } },
     "topics": { "telemetry": "…/telemetry", "status": "…/status",
                 "command": "…/command", "config": "…/config" },
     "sensors": [ { "sensor_key": "probe_1", "sensor_type": "temperature", "unit": "C" } ],
     "approved_at": "2026-06-17T00:00:00.000Z"
   }
   ```

4. The firmware validates `kind == "device_config"` + `status == "approved"`, persists the
   runtime MQTT config to NVS, switches to runtime mode, and reconnects with the approved
   credentials. From then on it boots straight into runtime mode (no re-claim) until a
   factory reset or operator-triggered re-bootstrap.

## Actuator control (switches / relays)

A device may expose controllable outputs (actuators) in addition to sensors. Control is
asymmetric: the backend publishes a command, the device applies it and reports the
resulting state back.

**Command** (backend → `{topic_base}/command`, QoS 1, not retained):

```json
{
  "type": "actuator_command",
  "actuator_key": "relay_1",
  "command": "on",
  "value": null,
  "command_id": "cmd-2f1c…",
  "ts": "2026-06-18T09:21:04Z"
}
```

- `command` is `on`, `off`, or `set`; `value` is required for `set` (e.g. a dimmer level).
- `command_id` is unique per command and should be echoed back for idempotent acks.

**State** (device → `{topic_base}/state`, retain recommended so late subscribers see it):

```json
{
  "actuators": [
    { "actuator_key": "relay_1", "state": "on" }
  ],
  "reported_at": "2026-06-18T09:21:05Z"
}
```

- The backend updates each actuator's `current_state` from this report and marks the
  outstanding command for that actuator as acknowledged.
- The device ACL grants write on `{topic_base}/state` and read on `{topic_base}/command`.

> **Deferred:** firmware does not yet subscribe to `command` or publish `state`. Until that
> ships, the app records `desired_state` and publishes commands, but `current_state` stays
> unconfirmed.

## Security posture (current)

- Per-device MQTT credentials, generated by the backend, encrypted at rest with
  AES-256-GCM. Broker ACLs are provisioned per device (flat-file or Mosquitto
  dynamic-security with default-deny).
- The backend broker layer is TLS-capable (`mqtts`); **the firmware is not yet** — it
  connects on `1883` only. Moving devices to `8883` + a pinned CA is the main remaining
  security gap.
- The bootstrap claim path is gated only by the shared bootstrap broker user + ACLs; there
  is no device-supplied nonce binding the approved reply to a specific claim yet.
