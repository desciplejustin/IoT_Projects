# IoT MQTT Integration Plan

> ⚠️ **Historical planning document.** The implemented wire contract differs from the
> payload/topic/SQL examples below. See [`CONTRACT.md`](CONTRACT.md) for the authoritative
> contract that matches the running firmware (`shared_libs/BBHIotFirmwareCore`) and backend
> (`bbh-app25.03/backend/src/services/iot*.ts`). Key differences: readings use `sensor_key`
> (not `sensor_code`) and carry no `reading_type`; there is no `state` topic; the device
> timestamp field is `reading_time` (not `recorded_at`); and the real schema is
> `iot_sensor_readings` with `(device_id, sensor_id, ingest_message_id)` dedup, not the
> `iot_readings` table shown here.

## Project Context

This document outlines how to add Mosquitto MQTT to the existing farm application stack:

- React frontend
- Express backend
- PostgreSQL database
- TimescaleDB for time-series IoT readings
- ESP32 / ESP8266 IoT devices

The goal is to let IoT devices publish sensor readings reliably into the main application, while the backend validates, stores, and serves the data to React dashboards and operational tools.

## Review Summary

The direction is good. The main improvements needed were:

- make MQTT the actual firmware contract, not just the backend transport
- add an explicit retained availability topic
- define payload fields for schema versioning and deduplication
- avoid depending on device wall-clock time unless NTP or RTC is present
- define topic and credential rules clearly enough for all future firmware in `IoT_Projects`

This version applies those improvements.

## Recommended Architecture

```text
ESP32 / ESP8266 Devices
        |
        | MQTT publish / subscribe
        v
Mosquitto MQTT Broker
        |
        | Express MQTT worker subscribes to device topics
        v
Express Backend
        |
        | Validate, enrich, deduplicate, store
        v
PostgreSQL / TimescaleDB
        |
        | REST API / WebSocket
        v
React Dashboard
```

## Responsibility Split

Devices are responsible for:

- sensor reads
- Wi-Fi reconnect
- MQTT reconnect
- telemetry publishing
- heartbeat / availability publishing
- local setup and diagnostics

Mosquitto is responsible for:

- transport
- retained availability state
- authenticated device sessions

Express is responsible for:

- subscribing to device topics
- validating payloads
- rejecting malformed or unauthorized data
- stamping server receive time when device time is absent
- deduplicating by `device_code` + `message_id`
- saving to PostgreSQL / TimescaleDB
- exposing React-facing APIs

React should not connect directly to Mosquitto in version 1.

## Broker Setup

Install on Debian:

```bash
sudo apt update
sudo apt install mosquitto mosquitto-clients
```

Enable and start Mosquitto:

```bash
sudo systemctl enable mosquitto
sudo systemctl start mosquitto
```

Check status:

```bash
sudo systemctl status mosquitto
```

For the first farm-network version:

- keep broker access LAN-only
- require username / password auth
- prefer per-device or per-device-class credentials
- restrict topic access where practical

## Topic Structure

Recommended device namespace:

```text
bbh/iot/{deviceCode}/telemetry
bbh/iot/{deviceCode}/status
bbh/iot/{deviceCode}/state
bbh/iot/{deviceCode}/command
bbh/iot/{deviceCode}/config
bbh/iot/{deviceCode}/ota
```

Examples:

```text
bbh/iot/coldroom-temp-001/telemetry
bbh/iot/coldroom-temp-001/status
bbh/iot/coldroom-temp-001/state
bbh/iot/coldroom-temp-001/command
```

Topic behavior:

- `telemetry`: sensor payloads, not retained
- `status`: JSON heartbeat payload, not retained
- `state`: simple retained availability payload such as `online` / `offline`
- `command`: backend-to-device commands
- `config`: backend-to-device config updates
- `ota`: firmware rollout control

Recommended MQTT semantics:

- retained last-will `offline` on `state`
- retained `online` on successful connect
- QoS 1 for telemetry and status where the client library supports it
- no shared topics published directly by devices

## Payload Contract

### Telemetry

Example topic:

```text
bbh/iot/coldroom-temp-001/telemetry
```

Example payload:

```json
{
  "schema_version": 1,
  "device_code": "coldroom-temp-001",
  "message_id": "coldroom-temp-001-381225-44",
  "firmware_version": "2.0.0",
  "uptime_seconds": 381225,
  "readings": [
    {
      "sensor_code": "probe_1",
      "reading_type": "temperature",
      "value": 24.6,
      "unit": "C"
    },
    {
      "sensor_code": "probe_2",
      "reading_type": "temperature",
      "value": 25.1,
      "unit": "C"
    }
  ]
}
```

Notes:

- `message_id` must be unique per device message
- `device_code` in the payload must match the topic
- `recorded_at` is optional and should only be trusted when the device has reliable time sync
- backend should stamp receive time when `recorded_at` is absent

### Status

Example topic:

```text
bbh/iot/coldroom-temp-001/status
```

Example payload:

```json
{
  "device_code": "coldroom-temp-001",
  "firmware_version": "2.0.0",
  "uptime_seconds": 381240,
  "wifi_rssi": -63,
  "ip_address": "10.24.16.41",
  "status": "online"
}
```

### State

Example retained payload:

```text
online
```

Last-will payload:

```text
offline
```

## Express MQTT Worker

Install the MQTT package in the backend:

```bash
npm install mqtt
```

Suggested backend structure:

```text
backend/
  src/
    app.js
    server.js
    db/
      pool.js
    modules/
      iot/
        mqttClient.js
        iotRoutes.js
        iotController.js
        iotService.js
        iotValidation.js
```

The backend should subscribe to:

```text
bbh/iot/+/telemetry
bbh/iot/+/status
bbh/iot/+/state
```

Backend rules:

- parse topic into `deviceCode` and `messageType`
- reject malformed topics
- verify payload `device_code` matches topic
- deduplicate telemetry by `device_code` + `message_id`
- upsert `last_seen_at` from any valid message
- store raw payload for diagnostics

## PostgreSQL / TimescaleDB Tables

### Devices Table

```sql
CREATE TABLE iot_devices (
    id SERIAL PRIMARY KEY,
    device_code VARCHAR(100) UNIQUE NOT NULL,
    device_name VARCHAR(150),
    location_name VARCHAR(150),
    device_type VARCHAR(100),
    board_type VARCHAR(100),
    mqtt_client_id VARCHAR(150),
    firmware_version VARCHAR(50),
    active BOOLEAN DEFAULT TRUE,
    last_seen_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ DEFAULT NOW()
);
```

### Sensors Table

```sql
CREATE TABLE iot_sensors (
    id SERIAL PRIMARY KEY,
    device_id INT REFERENCES iot_devices(id),
    sensor_code VARCHAR(100) NOT NULL,
    sensor_name VARCHAR(150),
    sensor_type VARCHAR(100),
    unit VARCHAR(50),
    active BOOLEAN DEFAULT TRUE,
    created_at TIMESTAMPTZ DEFAULT NOW(),
    UNIQUE(device_id, sensor_code)
);
```

### IoT Readings Table

```sql
CREATE TABLE iot_readings (
    id BIGSERIAL PRIMARY KEY,
    recorded_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    received_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    device_code VARCHAR(100) NOT NULL,
    message_id VARCHAR(150) NOT NULL,
    sensor_code VARCHAR(100) NOT NULL,
    reading_type VARCHAR(100) NOT NULL,
    reading_value DOUBLE PRECISION NOT NULL,
    unit VARCHAR(50),
    raw_payload JSONB,
    created_at TIMESTAMPTZ DEFAULT NOW(),
    UNIQUE(device_code, message_id, sensor_code)
);
```

Convert to TimescaleDB hypertable:

```sql
SELECT create_hypertable('iot_readings', 'recorded_at');
```

### Device Status Table

```sql
CREATE TABLE iot_device_status (
    id BIGSERIAL PRIMARY KEY,
    device_code VARCHAR(100) NOT NULL,
    mqtt_state VARCHAR(50),
    wifi_rssi INT,
    ip_address VARCHAR(50),
    firmware_version VARCHAR(50),
    uptime_seconds BIGINT,
    status VARCHAR(50),
    recorded_at TIMESTAMPTZ DEFAULT NOW(),
    raw_payload JSONB
);
```

## React Dashboard API Routes

React should continue calling Express routes such as:

```text
GET /api/iot/devices
GET /api/iot/devices/:deviceCode/latest
GET /api/iot/devices/:deviceCode/history?from=2026-06-01&to=2026-06-05
GET /api/iot/alerts
```

Recommended first dashboard view:

- device name
- latest reading per sensor
- last seen time
- online / offline state
- Wi-Fi strength, where available

## Firmware Roadmap

### Phase 1: Minimum Stable Firmware

Each device should support:

- Wi-Fi configuration
- MQTT broker configuration
- device code
- topic root
- telemetry publishing
- status heartbeat
- retained online / offline state
- reconnect logic
- basic LED state
- local diagnostics page

### Phase 2: Manageability

Add:

- local config page for MQTT and device settings
- long-press reset
- OTA updates
- config updates from backend
- command handling

### Phase 3: Advanced Features

Add:

- offline buffering
- retry queue for QoS / session recovery edge cases
- remote reboot
- firmware channels
- calibration settings
- alert rules

## First Development Sequence

1. Install Mosquitto on Debian.
2. Test broker manually with `mosquitto_sub` and `mosquitto_pub`.
3. Add the Express MQTT worker and subscribe to `telemetry`, `status`, and `state`.
4. Create PostgreSQL / TimescaleDB tables.
5. Confirm a test publish creates readings and device-status records.
6. Build the first ESP32 node with two DS18B20 probes.
7. Add the React dashboard for latest readings and device health.

## Manual Broker Test

Subscribe:

```bash
mosquitto_sub -h localhost -t "bbh/iot/+/telemetry"
```

Publish:

```bash
mosquitto_pub -h localhost -t "bbh/iot/coldroom-temp-001/telemetry" -m '{"schema_version":1,"device_code":"coldroom-temp-001","message_id":"test-1","firmware_version":"dev","uptime_seconds":10,"readings":[{"sensor_code":"probe_1","reading_type":"temperature","value":24.6,"unit":"C"}]}'
```

## Security Notes

- Do not expose Mosquitto directly to the internet without proper security.
- Keep the first deployment LAN-only.
- Use username / password authentication from day one.
- Move to TLS certificates and tighter ACLs before remote access.
- Prefer VPN or private network access for administration.

## Recommended First Version

Build the first version as simply as possible:

```text
1. Mosquitto running on Debian
2. Express subscribes to device topics
3. PostgreSQL / TimescaleDB stores readings and state
4. React displays latest readings and last-seen status
5. One ESP32 publishes 2x DS18B20 temperature readings over MQTT
```

## Final Decision

The system should follow this split:

```text
Devices publish MQTT messages.
Mosquitto transports and retains availability state.
Express validates, stores, and exposes APIs.
PostgreSQL / TimescaleDB stores the history.
React displays data and manages configuration through Express.
```

That keeps the IoT system scalable, firmware-friendly, and aligned with the long-term direction for this workspace.
