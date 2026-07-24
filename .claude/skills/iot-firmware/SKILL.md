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

## Keep this skill current (living doc)

**This skill is the workspace's durable memory for firmware work — grow it as we
learn.** Whenever a task turns up something reusable — a tool or command that
worked (e.g. how to scan/identify a connected board), a board quirk or strapping
gotcha, a contract limitation, a platform constraint, a flashing pitfall — **record
it here in the same change**, in the section it belongs to (bench tools, board
gotchas, backend deps, …). Prefer editing the relevant section over appending a
loose note; keep it concise and authoritative. If a discovery contradicts
something already written, **fix the stale text** instead of layering a caveat on
top — doc drift is the exact failure this repo keeps getting bitten by.

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

### Multi-chip: the platform-compat seam

The core is **one shared, chip-agnostic body of logic** that runs on both ESP32
and ESP8266. Everything that genuinely differs per chip is isolated in
`shared_libs/BBHIotFirmwareCore/src/platform/`:

- **`platform_compat.h`** — selected at compile time by `ARDUINO_ARCH_ESP32` /
  `ARDUINO_ARCH_ESP8266`. Aliases the persistence + web-server classes
  (`BbhPreferences`, `BbhWebServer`) and wraps the few per-chip calls:
  `bbhPlatformMacBytes` (factory MAC — **ESP32 byte order is preserved**, don't
  "simplify" it or every ESP32 device re-onboards), `bbhPlatformSetTxPowerDbm` /
  `bbhPlatformSetTxPowerApLow` / `bbhPlatformDisableModemSleep` (WiFi power/sleep),
  and `bbhPlatformFsBegin` (LittleFS mount).
- **`BbhPreferences8266.h`** — a LittleFS-backed drop-in for the ESP32
  `Preferences` API (same keys/methods), so the core persists config identically
  on ESP8266 (which has no NVS `Preferences`).

**Rules:** keep the core free of `#ifdef` — if you need a platform macro in
`BBHIotFirmwareCore.cpp`, add a wrapper in `platform_compat.h` instead. A change to
core logic then benefits every board automatically; adding a new chip family means
adding one branch in `platform_compat.h` (+ a backend shim like the 8266
Preferences one), never forking the core. This is why we do **one core + per-board
backends**, not a copied core per board (copies drift; a core fix would need N
manual edits).

**Board pin note:** ESP8266 NodeMCU pin labels are `D1/D2/…`, not raw GPIO. D3 =
GPIO0 and D4 = GPIO2 are **boot-strapping** pins (must be HIGH at boot) and D8 =
GPIO15 must be LOW — don't wire an input there that could be driven the wrong way
at power-up. See Project5 for a worked contactor-input pin map.

**MQTT read buffer must fit the whole `device_config` (silent onboarding block).**
The lwmqtt (256dpi/MQTT) client must fit an *entire incoming packet* in its read
buffer or it returns `LWMQTT_BUFFER_TOO_SHORT` (`err=-1`) and drops the link — so a
too-small read buffer blocks onboarding *invisibly*: the device connects,
subscribes, then the link dies before the retained `device_config` is delivered
(no `RX` log), looping connect→subscribe→drop→reconnect every ~5 s, never applying
the approved reply. Publishing is unaffected (payloads stream after a short
header), so *claims* send fine — the failure is receive-only, and it only bites
once a retained approved config exists and is large enough. The buffer is set in
the `BbhIotFirmwareCore` **constructor init list** `mqtt_(read, write)` — currently
`3072, 1024`. A real 4-sensor `device_config` is ~1.1 KB; the old `1024` read
buffer just missed it (1–2 sensor ESP32 devices fit, 4-sensor ESP8266 didn't).
When adding many sensors / long keys, keep read ≥ the retained config size. (An
init-list entry overrides any `{...}` default member initializer in the header —
edit the init list, not the header.)

**Filesystem must be mounted before any config read (ESP8266 persistence).** On
ESP8266 the config store (`BbhPreferences`) is LittleFS-backed, so `setup()` mounts
the FS (`bbhPlatformFsBegin()`) **before** `loadRuntimeConfig()` /
`loadAndIncrementBootSeq()`. If config is read before the mount, every read returns
blank: the device re-runs bootstrap/claim on **every** boot instead of going
straight to runtime, and `boot_seq` never advances (breaking the reboot-safe
`message_id`). The tell is `Boot sequence: 1` on every reboot. On ESP32 (NVS,
independent of LittleFS) the ordering is harmless — this is an ESP8266-only trap
introduced by the LittleFS-backed shim.

**ESP8266 PROGMEM gotcha (crashes the portal):** on ESP8266, a byte-wise read
from flash-resident `PROGMEM` data faults — `Exception (3)` / LoadStoreError, with
`excvaddr` in the `0x40200000+` (irom) range. ESP32 maps flash byte-addressable so
the same code runs fine there, which is why this only shows up on the 8266. The
core hit this once: a `PROGMEM` CSS blob handed to WiFiManager
`setCustomHeadElement(const char*)`, which `String`-concatenates it byte-by-byte
when rendering the captive portal → instant crash right after `*wm:Starting Web
Portal`. **Fix:** don't mark such buffers `PROGMEM` (plain `const char[]` lands in
byte-addressable `.rodata` — RAM on 8266, still flash on ESP32), or read via
`pgm_read`/`FPSTR`/`F()`. Rule of thumb: never pass a raw `PROGMEM` pointer to an
API that will `strlen`/`memcpy`/concat it. Verify a new ESP8266 device actually
reaches and survives the portal on real hardware — this class of bug builds
cleanly and only faults at runtime.

## Creating a new device firmware project

Work top-to-bottom. Mirror **Project4-C3-TempMonitor** (newest, cleanest example)
or **Project1-ColdRoom-TempMonitor** (the documented reference).

1. **Folder**: `Project{N}-{ShortName}/` with `src/main.cpp`, `platformio.ini`,
   `PROJECT.md`.

2. **Sensor adapter** — implement `BbhSensorAdapter`:
   - `begin()` — init the sensor bus.
   - `describeSensors(...)` — declare each sensor with a **deterministic**
     `sensor_key` (`probe_1`, `moisture_1`, …), a `sensor_type`, and a **canonical
     unit** (`C`, `%`, `pH`, `mS/cm`, `state` — anything else is quarantined by the
     backend).
   - `readTelemetry(...)` — sample and mark each reading `valid`/invalid.
   The `sensor_key`/`unit` you declare here are exactly what gets sent in the
   bootstrap claim, so they must match what the backend expects to register.
   - **Digital / on-off inputs** (contactor open-closed, door, float switch) use
     the canonical unit **`state`**, `value` exactly `0` or `1` (`1` =
     closed/energized/active) — CONTRACT.md → *Canonical units → `state`*. It rides
     the normal telemetry pipeline; don't confuse it with the actuator `…/state`
     topic (outputs the device drives). **Debounce every mechanical input:** sample
     continuously and hold the settled state, and have `readTelemetry()` return the
     held value. The core only calls `readTelemetry()` at the interval, so drive the
     debounce from a non-blocking `poll()` you call in the sketch `loop()` **before**
     `firmwareCore.loop()`. Worked example: `Project5-UV-Monitor` (`ContactorSampler`).

   - **Actuator devices** (relays/buzzers/lights the backend drives) *additionally*
     implement `BbhActuatorAdapter` and construct the core with the **3-arg**
     `BbhIotFirmwareCore(config, sensorAdapter, actuatorAdapter)`. They still need a
     sensor adapter with >=1 canonical sensor to onboard (a relay-position `state`
     sensor works). Full contract + worked example (`Project6-UV-Indicator`) in
     [Actuator devices](#actuator-devices-relays--buzzers--lights-the-backend-drives).

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
   - `[env:{name}_scan]` — a sensor-ID / wiring scan helper (`-D SENSOR_ID_SCAN_ONLY=1`)
     to discover 1-wire ROM codes / verify inputs before wiring the real firmware.
     **Add `lib_ignore = BBHIotFirmwareCore` to this env.** The scan sketch doesn't
     include the core, but `lib_extra_dirs` still makes PlatformIO compile it — and
     since the scan env omits the core's deps (ArduinoJson/MQTT/WiFiManager) the
     build dies with `fatal error: ArduinoJson.h: No such file`. `lib_ignore` skips
     the unused core. (Older scan envs without this line, e.g. Project4 `c3_scan`,
     are latently broken for the same reason.) **Actuator devices** use the same
     pattern for an **output** self-test instead: `[env:{name}_selftest]` with
     `-D SELFTEST_ONLY=1` + `lib_ignore = BBHIotFirmwareCore`, cycling each relay /
     buzzer / LED to verify wiring + polarity before onboarding (see
     `Project6-UV-Indicator`).
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
   `firmware_version`, `location_hint`, `ip_address`, `sensors[]`, and — for
   actuator devices — an optional `actuators[]`). At least one sensor with a
   canonical unit is **always** required (even when `actuators[]` is present);
   declared actuators auto-register on approval. See [Actuator devices](#actuator-devices-relays--buzzers--lights-the-backend-drives).
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
- **Telemetry cadence + logging.** The publish interval is
  `readingIntervalMinutes_` (default 5, whole minutes, set via the portal "Reading
  interval" field) — **not** `BbhTimingConfig.telemetryIntervalMs`, which is
  currently **dead/unused** (fill it to satisfy the struct, but don't expect it to
  do anything; cadence is minutes-granular, min 1). Each publish logs a
  `Telemetry posted -> {topic} : key=value, …` line (state units as 0/1) so posting
  is visible on serial — but only in **runtime** mode; a device still in
  bootstrap/claim is not posting. `logEvent()` prefixes a real UTC timestamp once
  NTP resolves, else uptime `millis`.
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
  chosen LED pins are actually wired on the target board (see gotchas). Either core
  LED pin (`blueLedPin`/`redLedPin`) may be set to **`-1`** to disable it — for a
  device with its own indicator scheme (e.g. `Project6-UV-Indicator` drives external
  RED=power / GREEN=WiFi LEDs from the sketch and disables the core red LED).
- **Local diagnostics** on port 80: `/` (HTML, Basic-Auth) + `/api/status` (JSON).
  Config forms post to `/setup`. Follow the existing slider/field pattern; validate
  and clamp every input server-side so a user can't submit an out-of-range value.

## Actuator devices (relays / buzzers / lights the backend drives)

A device with controllable **outputs** implements `BbhActuatorAdapter` in addition
to `BbhSensorAdapter` and uses the **3-arg constructor**
`BbhIotFirmwareCore(config, sensorAdapter, actuatorAdapter)`. Sensor-only devices
keep using the 2-arg constructor unchanged. The core then, in **runtime** mode:
subscribes to `{topic_base}/command`, routes each `actuator_command` to
`applyCommand()`, and publishes the full actuator set to `{topic_base}/state`
(retain=true) on every connect **and** after each command. Wire format is
authoritative in CONTRACT.md → *Actuator control* — don't re-document the fields.
**Worked example: `Project6-UV-Indicator`** (4 relays + 2 buzzers).

The adapter contract:
- `describeActuators(out, max, outCount)` — declare each output (`actuator_key`,
  `actuator_name`, `actuator_type`) for the claim's `actuators[]`.
- `applyCommand(key, command, value)` — apply `on`/`off`/`set` to the actuator
  named `key`; **return `true` only if the key matched** (the core ACKs matched
  commands, leaves unknown keys' commands open). Keep it non-blocking.
- `reportState(out, max, outCount)` — fill current state per actuator (`"on"`/`"off"`
  string literals); this is the ACK body + the retained connect publish.
- `tick()` — called every `loop()`; render **time-based** output patterns here (e.g.
  a buzzer's on/off pulse cadence). **Never block** — no `delay()`; gate on `millis()`.

**Onboarding an actuator device.** Actuators **are** declared in the bootstrap
claim's optional `actuators[]` (`describeActuators()` → core adds the array), so they
**auto-register on approval** — no manual Controls creation. But the claim is still
gated on **≥1 canonical sensor** (an actuators-only claim with no valid sensor is
rejected), so an actuator device must also declare an honest sensor: Project6 reports
each relay's commanded position as a `relay_N_state` **`state`** sensor. Keep the
feedback `sensor_key` (`relay_N_state`) **distinct** from the output `actuator_key`
(`relay_N`) — sensors land in `iot_sensors`, actuators in `iot_actuators`; an unknown
`actuator_type` falls back to `relay` backend-side. `state` is backend-whitelisted
(see below), so none of this needs a backend change. Wire format for `actuators[]` is
authoritative in CONTRACT.md → *Bootstrap / claim flow*.

**Own status-LED scheme?** The core's `blueLedPin`/`redLedPin` are now **optional**:
set a pin to **`-1`** to disable that core status LED. Project6 drives its own
external RED=power / GREEN=WiFi LEDs from the sketch `loop()` and sets
`redLedPin = -1`, mapping `blueLedPin` to the onboard LED as a health-blink only.

**Backend `state` unit has landed.** The `state` canonical unit (discrete 0/1) is
**whitelisted in `bbh-app25.03`** — verified in `backend/src/services/iotUnits.ts`
(`CANONICAL_UNITS` includes `'state'`, value-domain `{0,1}`, with tests). So
`state`-sensor devices (Project5 contactors, Project6 relay positions) onboard with
no backend change. The old `BACKEND-TASK-state-unit.md` handoff is complete; treat
any "state blocked on backend" note as stale.

## Board pins & strapping gotchas

- Choose LED + reset-button pins that are **not strapping pins** and are actually
  connected on your board.
- **ESP32-C3-DevKitM-1**: reset/BOOT button = **GPIO9**. The onboard LED is an
  addressable RGB on **GPIO8**, *not* a plain LED — the firmware's `red`/`blue`
  LED pins (GPIO5/6 on Project4) only light up if you wire discrete LEDs there.
- **Project1 esp32dev**: reset button is **GPIO0**. A serial reader that holds DTR
  low can trigger an *accidental* factory reset (wipes config) — beware when
  scripting the monitor. (The C3's native-USB reset on GPIO9 is not affected.)

## Bench: scan & identify a connected board

When a board is plugged in and you need to know *what it is* and *which port*,
do this before assuming anything (COM numbers move between sessions):

1. **List serial devices + decode the USB VID:PID** — the bridge chip and VID:PID
   usually tell you the board family:
   ```powershell
   Get-CimInstance Win32_PnPEntity | ? { $_.Name -match 'COM\d+' } |
     Select-Object Name, DeviceID, Manufacturer, Status | Format-List
   ```
   Known signatures on this bench:
   - `303A:1001` — Espressif **native USB** (ESP32-C3/S3, e.g. the C3 on COM10).
   - `10C4:EA60` — **Silicon Labs CP210x** UART bridge (classic ESP32 DevKitC /
     WROOM, or an ESP8266 NodeMCU).
   - `1A86:7523` — **CH340** UART bridge (the bench CH340 board).

   ⚠️ **Two CP210x boards now share the bench** — both enumerate as `10C4:EA60`, so
   a bare `hwgrep://10C4:EA60` may grab the wrong one. Disambiguate by DeviceID:
   Project5's NodeMCU has a programmed serial (`…EA60\0001`, on COM3); Project6's
   ESP32 WROOM-32 (MAC `a4:cf:12:9a:57:b0`) has a location-based ID (`…EA60\6&…`, on
   COM12). Keep explicit `upload_port`/`monitor_port` COM numbers for these two.

2. **Confirm the actual chip — safely — with esptool in ROM download mode.** This
   is the *safe* way to identify a board without running its firmware, so it can't
   trip firmware-side behavior like the GPIO0/DTR factory reset. `pio` may not be
   on PATH, but the PlatformIO-bundled esptool + venv Python always work:
   ```powershell
   $py = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
   $esptool = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"
   & $py $esptool --port COM3 --before default_reset --after hard_reset flash_id
   ```
   Reports chip type (e.g. `ESP8266EX`, `ESP32-C3`), MAC, crystal, and flash size.
   esptool puts the chip in the ROM bootloader, so the resident firmware never runs
   during detection (it hard-resets back to normal at the end).

3. **Prefer identity over COM number.** Once you know the VID:PID, pin the port by
   identity where possible (`upload_port = hwgrep://303A:1001`) so a re-enumerated
   COM number doesn't break the build.

> ℹ️ **The shared core runs on both ESP32 and ESP8266.** Chip-family differences
> live in one seam — `shared_libs/BBHIotFirmwareCore/src/platform/` — see
> [Multi-chip: the platform-compat seam](#multi-chip-the-platform-compat-seam).
> Adding another chip family = one branch there + a backend shim, never a core
> fork. (Project2 is still just a blink sketch, not a core user; Project5 is the
> first ESP8266 device on the core.)

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
- **Actuator devices:** declared `actuators[]` auto-registered on approval; a backend
  `actuator_command` on `{topic_base}/command` drives the output and the device ACKs
  on `{topic_base}/state` so the app's `current_state` updates and the command closes;
  actuator state is (re)published on every reconnect (retained), so a device that
  reconnects reflects reality; time-based outputs (buzzer pulse) keep rendering across
  a reconnect.

## Backend & broker dependencies (authoritative — from the BBH backend)

Broker provisioning + backend approval are owned by the `bbh-app25.03` backend
(`iotMqttProvisioner.ts`, `iotMqttBootstrap.ts`, `controllers/iot.ts`) and the
Mosquitto dynamic-security broker. Firmware must match these exactly.

1. **Per-device ACL** (dynsec `deviceRoleAcls`; `topic_base = bbh/iot/{device_key}`):
   - PUBLISH: `{topic_base}/telemetry`, `/status`, `/state`
   - SUBSCRIBE + RECEIVE: `{topic_base}/command`, `/config`
   - **Literal topics only — no wildcards, no cross-device access.** Firmware must
     publish/subscribe exactly these.

2. **Bootstrap account ACL** (shared account + client-id `bbh-iot-bootstrap`):
   - PUBLISH: `bbh/iot/bootstrap/claim`
   - SUBSCRIBE + RECEIVE: `bbh/iot/bootstrap/reply/#` (ACL grants the whole
     namespace; firmware subscribes only its own `…/reply/{hardware_id}`).
   - ⚠️ **Reply topic uses the SANITISED hardware_id** (every char outside
     `[A-Za-z0-9._-]` → `-`), e.g. MAC `60:8C:9B:C1:3D:E8` →
     `bbh/iot/bootstrap/reply/60-8C-9B-C1-3D-E8`. **Verified consistent:** the
     firmware's `bootstrapReplyTopic()` = `sanitizeMqttToken(hardwareId())`
     produces the dashed form and both subscribe (l.697) and message-match
     (l.1168) use it. The claim *payload* still carries the colon MAC; both sides
     sanitize independently. Don't "fix" this to colons — that would publish
     claims fine but never see the reply (looks exactly like "backend not
     responding").

3. **Reply retention & operator flow**:
   - pending `{kind:'claim_status',status:'pending',claim_code,…}` → QoS 1,
     **retain=false**
   - approved `{kind:'device_config',status:'approved',…}` → QoS 1, **retain=true**
     (so a device reconnecting later still gets its config)
   - Approval is **always MANUAL**, in the IoT app (Devices → pending-claims):
     `POST /api/iot/v1/device-claims/{claim_id}/approve {device_name,
     location_code?, notes?}`. Keyed by `claim_id` (UUID); the 6-digit `claim_code`
     is only for visually matching the physical device.

4. **Broker environment matrix** — both plaintext `:1883`, dynsec, prefix `bbh/iot`:
   - **DEV**: WSL2 Mosquitto reached at the **Windows host LAN IP**, currently
     **`10.24.16.105`** (LAN devices reach it via `netsh portproxy`).
     ⚠️ **DHCP-volatile** — this IP moves; a stale value (`.94`) was the exact bug
     behind a dead portproxy earlier. Firmware selects dev-vs-live by broker IP, so
     keep the dev IP trivial to change and always point it at the host's *current*
     LAN IP (or just set it in the setup portal, which overrides the compiled
     default). See [dev broker IP](#dev-broker-ip-is-dhcp-volatile).
   - **LIVE**: BeagleBone **`10.24.16.176`**, dynsec (TLS is the target, not yet
     deployed).

5. **Topic prefix + canonical units**: `IOT_MQTT_TOPIC_PREFIX = bbh/iot`; suffixes
   `telemetry`/`status`/`state`/`command` + literal `config`. Canonical units
   `C`, `%`, `pH`, `mS/cm`, `state` are **enforced at claim time** — a sensor with a
   non-canonical unit is dropped, and a claim with zero valid sensors is rejected
   outright. So a wrong `unit` in your sensor adapter silently kills onboarding.
   ✅ `state` (discrete boolean, `value` 0/1) **is now whitelisted** — verified in
   `bbh-app25.03` `backend/src/services/iotUnits.ts` (`CANONICAL_UNITS` includes
   `'state'`; value-domain `{0,1}`; passing tests). So `state`-sensor devices
   (Project5 contactors, Project6 relay positions) onboard with **no backend
   change**. The old `BACKEND-TASK-state-unit.md` handoff is complete — treat any
   "state blocked on backend" note as stale. `state` rides the existing
   `…/telemetry` ACL (no broker change).

6. **Sensor-type registration** — **not pre-registered.** Approval inserts each
   claim sensor into `iot_sensors` (`ON CONFLICT device_id+sensor_key DO UPDATE`).
   `sensor_type` is free-text (no registry/FK); only `unit` must be canonical.
   Thresholds default NULL; `sensor_name` defaults `"{device_name} {sensor_key}"`.

6b. **Actuator registration (from the claim)** — the claim's optional `actuators[]`
   is consumed at approval and each entry is registered in `iot_actuators` (keyed by
   `actuator_key`), so declared outputs need **no manual Controls creation**. Rules:
   `actuators[]` is optional (omit for sensor-only devices) but the ≥1-canonical-sensor
   gate still applies; a hint with a **blank `actuator_key`** is dropped; an **unknown
   `actuator_type`** falls back to `relay` (UI hint only); keep the feedback
   `sensor_key` (`relay_1_state`) **distinct** from the output `actuator_key`
   (`relay_1`). Control runtime: backend → `{topic_base}/command` (`actuator_command`),
   device applies + ACKs on `{topic_base}/state`; the per-device ACL already grants
   read on `/command` + write on `/state`, so **no broker change** for actuators.

7. **Re-bootstrap identity** — the same `hardware_id` re-claims to the **same**
   `device_key`, and existing credentials are **reused** when recoverable (fresh
   creds only for a brand-new device). Re-approval is still MANUAL.

8. **Credential rotation** (`POST /device-claims … rotate` → `rotateDeviceToken`)
   — graceful, never de-registers a live device:
   - Requires the device **currently connected** (else `409`, nothing changes).
   - Backend pushes a `device_config`-shaped message **plus top-level
     `rotation_id`** to `{topic_base}/config` (QoS 1, retain=false).
   - Firmware applies the new creds, then publishes to `{topic_base}/state`:
     `{ "event": "config_applied", "rotation_id": "{same id}" }` (this is
     `publishConfigAck()` — verified present).
   - Backend swaps broker password + DB + retained reply **only after that ACK**;
     no ACK within timeout ⇒ **full rollback**, device keeps old creds.

### Dev broker IP is DHCP-volatile

The dev broker is WSL2 Mosquitto behind the Windows host's LAN IP, which changes on
DHCP renewal. The compiled `BBH_BOOTSTRAP_BROKER_HOST` in each dev `platformio.ini`
is only a first-boot convenience — the setup portal's "MQTT broker IP" field
overrides it at runtime and persists to NVS. When onboarding a fresh dev device and
claims never get a reply, **first suspect a stale broker IP**: check the host's
current LAN IP (`ipconfig` → the `10.24.16.x` address) and either set it in the
portal or rebuild with the corrected `-D BBH_BOOTSTRAP_BROKER_HOST`.
