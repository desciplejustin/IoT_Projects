---
name: iot-firmware
description: >-
  Scaffold and develop BBH IoT device firmware consistently against the shared
  BBHIotFirmwareCore library and the authoritative MQTT wire contract. Use when
  creating a new Project{N} firmware, adding a temperature/humidity/actuator
  telemetry node, wiring the bootstrap/claim onboarding flow, writing a sensor
  adapter, or setting up platformio.ini / PROJECT.md for an ESP32 / ESP32-C3 /
  ESP8266 device in this workspace.
---

# BBH IoT Firmware Development

This workspace builds many small telemetry/actuator nodes that all share **one
firmware core** and speak **one MQTT wire contract**. The goal of this skill is
that every new device looks and behaves like the last one: same onboarding, same
topics, same persistence, same diagnostics UI, same failure LEDs.

## Golden rule: the contract is authoritative, don't re-invent it

Read these before changing anything on the wire. **Do not duplicate their field
definitions into new docs or code comments** — this repo has already been bitten
by doc drift (see the warnings at the top of the two baseline files):

- **[CONTRACT.md](../../../CONTRACT.md)** — the *authoritative* MQTT wire contract
  (topics, telemetry/status payloads, bootstrap/claim flow, actuator control).
  When anything disagrees with it, **CONTRACT.md wins**.
- **[FIRMWARE_BASELINE.md](../../../FIRMWARE_BASELINE.md)** — operational + security
  baseline (onboarding UX, LED behavior, factory reset, reconnect rules,
  verification checklist). Its *wire-format* fields are superseded by CONTRACT.md;
  its *operational* guidance is still current.
- Each device's **`PROJECT.md`** — the device-specific scope (sensors, pins,
  wiring, intervals, deviations). Every project has one; keep it accurate.

If a change genuinely alters the wire format, update **CONTRACT.md first**, then
the firmware, then note any backend/broker coordination needed (see
[Backend & broker dependencies](#backend--broker-dependencies)).

## Architecture: two layers

1. **Shared core** — `shared_libs/BBHIotFirmwareCore/` (`BBHIotFirmwareCore.h/.cpp`).
   Owns WiFi/captive-portal onboarding, bootstrap→runtime mode switching, NVS
   persistence, MQTT connect/reconnect, telemetry buffering, NTP, the local
   diagnostics web UI, factory reset, and LED state. **Changes here affect every
   device** — treat it as stable, shared API.
2. **Per-project glue** — `Project{N}-{Name}/src/main.cpp`. Provides only:
   - a `BbhSensorAdapter` subclass (the device's actual sensors), and
   - a `BbhFirmwareCoreConfig` instance (identity, pins, timings, bootstrap defaults).

The project's `setup()`/`loop()` just call `firmwareCore.setup()` / `firmwareCore.loop()`.

## Creating a new device firmware project

Work top-to-bottom. Mirror **Project4-C3-TempMonitor** (newest, cleanest example)
or **Project1-ColdRoom-TempMonitor** (the documented reference).

1. **Folder**: `Project{N}-{ShortName}/` with `src/main.cpp`, `platformio.ini`,
   `PROJECT.md`.

2. **Sensor adapter** — implement `BbhSensorAdapter`:
   - `begin()` — init the sensor bus.
   - `describeSensors(...)` — declare each sensor with a **deterministic**
     `sensor_key` (`probe_1`, `moisture_1`, …), a `sensor_type`, and a **canonical
     unit** (`C`, `%`, `pH`, `mS/cm` — anything else is quarantined by the backend).
   - `readTelemetry(...)` — sample and mark each reading `valid`/invalid.
   The `sensor_key`/`unit` you declare here are exactly what gets sent in the
   bootstrap claim, so they must match what the backend expects to register.

3. **`BbhFirmwareCoreConfig`** — fill every field: `firmwareVersion`
   (`{name}-x.y.z`), `setupApPrefix` (`{NAME}-SETUP`), `bootstrap` defaults, `ui`
   creds (from secrets — see below), `pins`, and `timing`. See
   [pins](#board-pins--strapping-gotchas) for pin selection.

4. **`platformio.ini`** — copy the Project4 pattern:
   - `[platformio]` → `default_envs` + `extra_configs = ..\shared_libs\bbh_secrets.ini`.
   - `[env]` base: `platform`, `board`, `framework`, `monitor_speed = 115200`,
     `lib_extra_dirs = ..\shared_libs`, `board_build.filesystem = littlefs`, and a
     **pinned** `upload_port` / `monitor_port` (this bench has multiple serial
     boards — never rely on auto-detect; see [flashing](#build-flash-monitor)).
   - `[env:{name}_scan]` — a sensor-ID scan helper (`-D SENSOR_ID_SCAN_ONLY=1`) to
     discover 1-wire ROM codes / addresses before wiring the real firmware.
   - `[env:{name}_app]` — full firmware. First build flag is
     `${bbh_secrets.build_flags}`; then non-secret per-project values
     (`BBH_BOOTSTRAP_BROKER_HOST/PORT`, `BBH_BOOTSTRAP_CLAIM_TOPIC`,
     `BBH_BOOTSTRAP_REPLY_PREFIX`), plus board flags (for ESP32-C3:
     `ARDUINO_USB_MODE=1`, `ARDUINO_USB_CDC_ON_BOOT=1`). Add
     `-D BBH_WIFI_HIGH_PERFORMANCE=1` only for mains-powered devices.

5. **Register** the project in `projects.registry.json` (name, path, description,
   board, platform, framework, defaultEnv).

6. **`PROJECT.md`** — document sensor inventory, board/wiring, pins, intervals,
   bootstrap defaults, and any baseline deviation. Follow Project1's structure.

## Secrets — never commit them

Shared credentials live in **`shared_libs/bbh_secrets.ini`** (gitignored, single
source of truth for all devices). It defines build flags for the setup-AP
password, the local dashboard login, and the shared bootstrap MQTT identity.
Reference it via `${bbh_secrets.build_flags}`; **never** hard-code these values in
`main.cpp` or a committed `platformio.ini`. Per-build overrides go in a gitignored
`platformio_override.ini`. `.gitignore` already covers both — keep it that way.

## Onboarding / registration flow (device side)

This is the part that must stay identical across devices. Full payloads are in
CONTRACT.md → *Bootstrap / claim flow*; the device-side sequence:

1. Boot with no final config → **BOOTSTRAP mode**. WiFiManager captive portal
   (`{NAME}-SETUP`) collects WiFi + bootstrap broker host.
2. Connect to the **bootstrap broker** with the shared bootstrap credentials.
3. Publish a **claim** to `bbh/iot/bootstrap/claim` (`hardware_id` = sanitized MAC,
   `firmware_version`, `location_hint`, `ip_address`, `sensors[]`). At least one
   sensor with a canonical unit is required.
4. Subscribe to `bbh/iot/bootstrap/reply/{hardware_id}`; receive `claim_status`
   *pending* (carries a `claim_code` an operator uses to approve), then a
   `device_config` *approved* message.
5. `applyApprovedConfig()` validates `kind == "device_config"` + `status ==
   "approved"`, persists the per-device MQTT config to NVS, switches to **RUNTIME
   mode**, and reconnects with the approved credentials.
6. Thereafter it boots straight into runtime mode until factory reset or an
   operator-triggered re-bootstrap.

The approved config the firmware consumes: `device{key,name,type,location_code,
expected_interval_minutes,offline_after_minutes}`, `mqtt{username,password,
client_id,topic_base,qos,keepalive_seconds,broker{host,port,protocol}}`,
`topics{status,telemetry,command,config}`, `sensors[]`, `approved_at`. Any of the
core MQTT/topic fields missing → the config is rejected and the device stays in
bootstrap.

## Non-negotiable firmware conventions

- **Reboot-safe `message_id`.** `message_id = {device_key}-{boot_seq}-{counter}`,
  where `boot_seq` is persisted in NVS and incremented every boot. The backend
  dedups on `(device_id, sensor_id, message_id)`. **Never ship a counter that
  restarts at boot without a per-boot prefix** — it causes silent post-reboot data
  loss.
- **Store-and-forward.** Telemetry is written to a bounded LittleFS ring buffer and
  flushed oldest-first on reconnect; `message_id` + `reading_time` are assigned at
  sample time and stored with the record, so identity/order survive outages and
  reboots.
- **NVS persistence.** All runtime config survives reboot (Preferences namespace
  `coldroom`). New persisted settings: add a `getX`/`putX` pair (load in
  `loadRuntimeConfig`, save in `persistBootstrapConfig`/`persistFinalConfig`) and a
  clamp on load — see `wifiTxPower_` for the pattern.
- **Non-blocking loop.** No long blocking waits in `loop()`; reconnect windows are
  timed via `millis()`. The only acceptable blocking delays are right before an
  intentional `ESP.restart()`.
- **LEDs**: red blink while connecting, blue solid when WiFi+MQTT healthy, red
  solid on failure. Factory reset (long-press) blinks then restarts. Confirm the
  chosen LED pins are actually wired on the target board (see gotchas).
- **Local diagnostics** on port 80: `/` (HTML, Basic-Auth) + `/api/status` (JSON).
  Config forms post to `/setup`. Follow the existing slider/field pattern; validate
  and clamp every input server-side so a user can't submit an out-of-range value.

## Board pins & strapping gotchas

- Choose LED + reset-button pins that are **not strapping pins** and are actually
  connected on your board.
- **ESP32-C3-DevKitM-1**: reset/BOOT button = **GPIO9**. The onboard LED is an
  addressable RGB on **GPIO8**, *not* a plain LED — the firmware's `red`/`blue`
  LED pins (GPIO5/6 on Project4) only light up if you wire discrete LEDs there.
- **Project1 esp32dev**: reset button is **GPIO0**. A serial reader that holds DTR
  low can trigger an *accidental* factory reset (wipes config) — beware when
  scripting the monitor. (The C3's native-USB reset on GPIO9 is not affected.)

## Build, flash, monitor

Prefer the VS Code PlatformIO toolbar (build ✓ / upload → / monitor 🔌) — portable
across machines. From the CLI, `pio` may not be on PATH; use the PlatformIO-
installed executable directly. On this machine that is
`C:\Users\Justin\AppData\Roaming\Python\Python314\Scripts\pio.exe`.

```powershell
# from the project folder, e.g. Project4-C3-TempMonitor
pio run -e {name}_app                 # build
pio run -e {name}_app -t upload       # build + flash
pio device monitor -e {name}_app      # serial monitor (115200)
```

- **COM ports move.** This bench has several USB-serial boards; the C3 re-enumerated
  from COM9→COM10 between sessions. If upload fails with "could not open COMx",
  scan for the device and update `upload_port`/`monitor_port`:
  `Get-CimInstance Win32_PnPEntity | ? { $_.Name -like "*COM*" }`. The C3's native
  USB is VID:PID `303A:1001` — you can pin `upload_port = hwgrep://303A:1001`
  instead of a fixed COM number.
- **"Access is denied" / port busy** on upload almost always means a **serial
  monitor is holding the port**. Find and stop it, then re-upload:
  `Get-CimInstance Win32_Process -Filter "Name='pio.exe'" | select ProcessId,CommandLine`
  then `Stop-Process -Id <pid> -Force`.

## Verification checklist (before calling a device done)

Run the FIRMWARE_BASELINE.md checklist end-to-end, at minimum:

- Onboards via captive portal; reconnects after WiFi loss; reconnects/resubscribes
  after MQTT disconnect.
- Persisted WiFi + MQTT + device settings survive a power cycle.
- Claim → pending → approved → runtime transition works, and the device boots
  straight to runtime on the next power cycle.
- Telemetry arrives on `{topic_base}/telemetry` matching CONTRACT.md; status
  heartbeat on `{topic_base}/status`.
- `message_id` continuity across a reboot (no dropped/duplicated readings) — verify
  `boot_seq` incremented.
- Local auth-protected status UI reachable and reflects live WiFi/MQTT/sensor state.
- Factory reset clears WiFi + stored config and returns the device to bootstrap.

## Backend & broker dependencies

The firmware side of onboarding is fully specified above and in CONTRACT.md. The
matching **broker provisioning** and **backend approval** behavior is owned by the
`bbh-app25.03` backend + the Mosquitto broker and must stay in lockstep. The items
below are what a new device relies on; confirm/record them here as they're
established so future projects inherit a complete picture:

- **Broker environment matrix** — authoritative dev vs live broker host/port and
  TLS status. (Known so far: dev `10.24.16.94`, live `10.24.16.176` (BeagleBone,
  dynamic-security); firmware is `1883`-only, no TLS yet.)
- **Bootstrap account ACL** — exact publish/subscribe grants the shared
  `bbh-iot-bootstrap` account needs (`bbh/iot/bootstrap/claim` publish;
  `bbh/iot/bootstrap/reply/{hardware_id}` subscribe).
- **Per-device account ACL** — the read/write topic list provisioned on approval
  (expected: publish `telemetry`/`status`/`state`, subscribe `command`/`config`
  under `{topic_base}`). Firmware and broker ACL must agree exactly.
- **Reply retention & lifecycle** — is the approved `device_config` published
  **retained** (so a device reconnecting later still receives it)? What is the
  operator pending→approved flow, and where is the `claim_code` entered?
- **Topic prefix + canonical units** — confirm `IOT_MQTT_TOPIC_PREFIX = bbh/iot`
  matches firmware, and the canonical unit set (`C`, `%`, `pH`, `mS/cm`).
- **Sensor-type registration** — must new `sensor_type`s be pre-registered
  backend-side before a claim carrying them will be approved, or are they created
  from the claim?
- **Re-bootstrap identity** — does the same `hardware_id` re-claim to the same
  `device_key`, and is re-approval automatic or manual?
- **Credential rotation** — the runtime `config` topic / `handleRuntimeConfigMessage`
  rotation path and its message format (note: rotating a live device's credentials
  must not de-register it — this has bitten us before).
