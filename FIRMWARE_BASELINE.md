# Firmware Baseline Contract

Reference implementation: `Project3-ColdRoom-TempMonitor`

Use this baseline for all future telemetry firmware unless a project explicitly documents a deviation in `PROJECT.md`.

## API and Transport Contract

- Base ingestion endpoint path: `/api/iot/v1/readings`
- Current backend host/port in this environment: port `5010`
- Claim onboarding endpoint path: `/api/iot/v1/device-claims`
- Required telemetry headers:
  - `Content-Type: application/json`
  - `Accept: application/json`
  - `x-device-token: <approved token>`

## Telemetry Payload Contract

Every telemetry message must include:

- `device_key` (stable and explicit)
- `message_id` (unique per message)
- `readings` array with one entry per sensor:
  - `sensor_key`
  - `value`
  - `unit`

Sensor and unit rules:

- Sensor keys must be deterministic (`probe_1`, `probe_2`, etc.)
- Units must be canonical backend units only: `C`, `%`, `pH`, `mS/cm`

## Device Claim Lifecycle Contract

Claim flow must support create and poll lifecycle:

1. Create claim (`POST /api/iot/v1/device-claims`)
2. Poll claim (`GET /api/iot/v1/device-claims/{claim_id}`)
3. Handle statuses and recovery:
   - `pending`
   - `approved`
   - `expired`
   - `rejected`
   - `claim_not_found` (recreate claim)

On approval, device must store:

- `device_key`
- `device_token`
- location metadata (if provided)

Stored credentials must survive reboot (NVS/Preferences).

## Operational UX Contract

- Captive portal onboarding must be available for first-time setup and Wi-Fi recovery.
- Local diagnostics server must run on port `80`.
- Local diagnostics endpoints:
  - `/` (HTML dashboard)
  - `/api/status` (JSON)
- Local dashboard auth is required (Basic Auth).

LED behavior contract:

- Red blink while connecting
- Blue when connected/success
- Red solid on failure timeout

Factory reset contract:

- Long-press reset must be non-blocking
- Must clear Wi-Fi credentials and stored device credentials

## Timing Contract

- Default sample/send interval: `30s`
- If claim approval returns `expected_interval_minutes`, firmware should adopt it.

## Production-Ready Verification Checklist

- Device onboards via captive portal and reconnects after Wi-Fi loss.
- Claim approval stores credentials and survives reboot.
- Telemetry returns HTTP success and backend `saved_count` updates.
- Local auth-protected status UI is reachable and reflects live state.
- Sensor mapping is stable and deterministic.
