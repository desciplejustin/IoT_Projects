# Project5 — UV Monitor

## Purpose

Monitors the **open/closed state of 4 contactors** in a UV system and reports
each as a discrete `state` telemetry sensor on the shared BBH wire contract. This
is a state-monitoring node (read-only digital inputs), not a measurement or
actuator node.

## Hardware

- **Board:** NodeMCU v2 (`nodemcuv2`, ESP8266EX, CP210x USB-UART, 4 MB flash)
- **Platform / framework:** `espressif8266` / Arduino
- **Firmware core:** shared `BBHIotFirmwareCore` via its ESP8266 backend
  (`platform/platform_compat.h` + `platform/BbhPreferences8266.h`). This is the
  first ESP8266 device on the shared core — see [First ESP8266 on the shared
  core](#first-esp8266-on-the-shared-core).

## Inputs / pin map

| Sensor key    | NodeMCU | GPIO | Strapping | Notes                          |
|---------------|---------|------|-----------|--------------------------------|
| `contactor_1` | D1      | 5    | no        |                                |
| `contactor_2` | D2      | 4    | no        |                                |
| `contactor_3` | D5      | 14   | no        |                                |
| `contactor_4` | D6      | 12   | no        | **moved off D3/GPIO0** (below) |

Support pins:

| Function            | NodeMCU | GPIO | Notes                                        |
|---------------------|---------|------|----------------------------------------------|
| Blue LED (healthy)  | D7      | 13   | wire a discrete LED                          |
| Red LED (fault)     | D0      | 16   | wire a discrete LED                          |
| Factory-reset button| D3      | 0    | onboard **FLASH** button (active-low, long-press) |

### Why the 4th input is on D6, not D3

The request was D1/D2/D3/D5, but **D3 = GPIO0 is a boot-strapping pin**. A closed
contactor pulling GPIO0 LOW at power-up forces the ESP8266 into serial-flash mode
and it will not boot. The 4th input is therefore on **D6 (GPIO12)** — electrically
identical at runtime, no boot hazard. GPIO0/D3 is instead reused for the
factory-reset button (the onboard FLASH button is already wired to GPIO0 and is
only pressed intentionally at runtime, so its strapping role is never triggered).

## Wiring & polarity

Each contactor's **dry auxiliary contact** connects its GPIO to **GND**. Inputs use
`INPUT_PULLUP`:

- contact **OPEN**  → pin reads **HIGH** → reported `value = 0`
- contact **CLOSED** → pin pulled to GND, reads **LOW** → reported `value = 1`

`value = 1` means the monitored contactor is **closed / energized**. If a given
contactor exposes a **normally-closed (NC)** aux instead, set `invert = true` for
that channel in `kInputs[]` (`src/main.cpp`) so "energized" still maps to 1. No
external pull-ups or resistors are required for the inputs (internal pull-ups are
used); LEDs need their usual series resistor.

## Debouncing (held stable state)

Contactor aux contacts bounce and can chatter. The firmware samples all 4 inputs
**every loop iteration** (`ContactorSampler::poll()`, non-blocking) and only
commits a change to the held state once a level has been steady for
**`kDebounceMs` (50 ms)**. `readTelemetry()` returns the currently-held stable
state, so the value published each interval is always the settled one — a bouncing
or momentarily-chattering contact never emits a spurious reading. Raise
`kDebounceMs` if a noisy contactor still chatters.

## Telemetry

- Each input → one `state` sensor (`sensor_type = "contactor"`, `unit = "state"`,
  `value ∈ {0,1}`), published on `{topic_base}/telemetry` like any other sensor,
  with `message_id` dedup and store-and-forward. See CONTRACT.md → *Canonical
  units → `state`*.
- **Reporting cadence:** the held state of all 4 inputs is sent every
  **`readingIntervalMinutes_`** — default **5 minutes**, settable in the setup
  portal ("Reading interval (minutes)", whole minutes, 1–1440). (The core does
  **not** use `BbhTimingConfig.telemetryIntervalMs`; that field is currently dead.)
  Detection latency is therefore bounded by that interval — minimum 1 minute. If
  you need sub-minute reaction or publish-on-change, that's a core enhancement (a
  sub-minute cadence or an adapter "state changed" hook), deliberately out of scope
  here since the brief was to send the stable state at the set interval.
- **Post logging:** each telemetry cycle logs a line like
  `… Telemetry posted -> bbh/iot/<key>/telemetry : contactor_1=1, contactor_2=0, …`
  (state shown as 0/1), so you can confirm posting on the serial monitor. Only
  appears in **runtime** mode (after onboarding) — in bootstrap mode the device is
  still claiming, not posting. Use `uvmon_scan` to watch live input states before
  onboarding.
- Status heartbeat every 60 s on `{topic_base}/status`.

## Bootstrap defaults

- Setup-AP prefix: `UVMON-SETUP`
- Firmware version: `uv-monitor-1.0.0`
- Location hint: `UV System`
- Dev bootstrap broker: `10.24.16.105:1883` (DHCP-volatile — override in the setup
  portal if claims get no reply).

## Baseline deviations

- **ESP8266, not ESP32** — status LEDs are on discrete GPIOs (the onboard LED is a
  strapping/active-low pin and is unused); the factory-reset button uses the
  onboard FLASH button on GPIO0. Persistence uses the LittleFS-backed
  `BbhPreferences8266` shim in place of ESP32 NVS (same keys, same behavior).
- **`state` unit requires backend support** — the backend must whitelist the
  `state` canonical unit before this device can onboard (see the backend task
  doc). No broker change is needed. Until then the claim is rejected as having no
  valid sensor.

## First ESP8266 on the shared core

The shared core was ESP32-only (used `Preferences`, ESP32 `WiFi.h`,
`esp_wifi_set_max_tx_power`). It now runs on ESP8266 through a compile-time
platform-compat seam (`shared_libs/BBHIotFirmwareCore/src/platform/`) that aliases
the persistence + web-server classes and wraps WiFi TX-power/sleep, the factory
MAC, and the LittleFS mount per chip family. Core logic is unchanged and shared;
selecting the board (`ARDUINO_ARCH_ESP8266` vs `ARDUINO_ARCH_ESP32`) picks the
backend. A future core change benefits ESP32 and ESP8266 devices alike.

## Current status

Firmware scaffolded against the shared core. Pending: backend `state`-unit support,
then flash (`uvmon_app`), verify wiring with `uvmon_scan`, and onboard.
