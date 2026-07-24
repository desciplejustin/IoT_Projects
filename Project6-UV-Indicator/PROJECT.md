# Project6 — UV Indicator

## Purpose

An **actuator / annunciator panel** for a UV system: the backend drives its
outputs over the shared BBH wire contract. Unlike Projects 1–5 (telemetry nodes),
this device's primary job is to **receive commands and act**, not to measure. It is
the **first device to use the core's actuator path** (CONTRACT.md → *Actuator
control*): the backend publishes an `actuator_command` to `{topic_base}/command`,
the firmware applies it and ACKs the resulting state on `{topic_base}/state`.

## Hardware

- **Board:** ESP32 WROOM-32 (`esp32dev`, ESP32-D0WDQ6, CP210x USB-UART, 4 MB flash;
  bench MAC `a4:cf:12:9a:57:b0`, enumerated COM12)
- **Platform / framework:** `espressif32` / Arduino
- **Firmware core:** shared `BBHIotFirmwareCore` (ESP32/NVS backend)

## Outputs

### Actuators (backend-controlled)

These are **declared in the bootstrap claim's `actuators[]`** and auto-register on
approval — the operator only renames/arranges them, no manual creation. They are
keyed by the exact `actuator_key` below (distinct from the `relay_N_state` feedback
sensor keys).

| actuator_key | Type   | GPIO | Silkscreen | Behaviour                            |
|--------------|--------|------|------------|--------------------------------------|
| `relay_1`    | relay  | 16   | D16        | on/off                                |
| `relay_2`    | relay  | 17   | D17        | on/off                                |
| `relay_3`    | relay  | 18   | D18        | on/off                                |
| `relay_4`    | relay  | 19   | D19        | on/off                                |
| `buzzer_1`   | buzzer | 25   | D25        | while ON: pulses **1.0 s on / 1.0 s off** |
| `buzzer_2`   | buzzer | 26   | D26        | while ON: pulses **0.5 s on / 0.5 s off** |

> This board's silkscreen labels pins `D<n>` where **n is the GPIO number**
> (D18 = GPIO18), so the two columns above are the same number.

`command: "on"` starts the output (a buzzer begins its pulse cadence immediately);
`command: "off"` stops it. `command: "set"` treats a truthy `value`
(`on`/`1`/`true`) as ON, so a generic switch control also works. The buzzer pulse
cadence is rendered locally and non-blocking (`IndicatorOutputs::tick()`), so the
backend only ever sends on/off — it does not toggle at the pulse rate.

### Status LEDs (local, NOT backend-controlled)

| Function            | GPIO | Silkscreen | Behaviour                        |
|---------------------|------|------------|----------------------------------|
| **RED** = power OK  | 23   | D23        | lit whenever the MCU is powered and running |
| **GREEN** = WiFi    | 22   | D22        | lit while associated to WiFi     |

These are driven from the sketch (`updateStatusLeds()`), independent of the core's
status-LED scheme. The core's `blueLedPin` is pointed at the **onboard LED (GPIO2)**
as a health-blink diagnostic; its `redLedPin` is **disabled (-1)** so it doesn't
clash with the RED=power LED.

### Support pins

| Function             | GPIO | Silkscreen | Notes                                  |
|----------------------|------|------------|----------------------------------------|
| Factory-reset button | 14   | D14        | momentary to GND, `INPUT_PULLUP`, long-press |
| Core health LED      | 2    | D2         | onboard LED (`blueLedPin`); optional diagnostic |

GPIO14 was chosen for reset (not the GPIO0 BOOT button) to avoid the GPIO0/DTR
accidental-factory-reset trap a serial monitor can trigger. If no button is wired,
the pull-up holds it inactive.

## Output polarity (verify before deploy)

Set at the top of `src/main.cpp`:

- `RELAY_ACTIVE_LOW = true` — most opto-isolated relay boards energise on a **LOW**
  drive. If relays click on at boot or the on/off sense is inverted, flip this.
- `BUZZER_ACTIVE_LOW = false` — buzzers via a transistor/driver are usually
  active-high.

`IndicatorOutputs::begin()` drives every output to its **OFF** state at boot, so a
correctly-set polarity means no relay clatter / no buzzer chirp on power-up.

## Onboarding — why the relays are also `state` sensors

The claim requires **≥1 sensor with a canonical unit** — and that gate applies even
when the claim declares actuators. So the firmware also declares each relay's
**commanded position** as a discrete `state` sensor:

| sensor_key      | sensor_type      | unit    | value            |
|-----------------|------------------|---------|------------------|
| `relay_1_state` | relay_feedback   | `state` | 1 = energised, 0 = off |
| `relay_2_state` | relay_feedback   | `state` | ″ |
| `relay_3_state` | relay_feedback   | `state` | ″ |
| `relay_4_state` | relay_feedback   | `state` | ″ |

This satisfies onboarding **and** gives relay-position history the backend can chart
/ automate on. The `state` unit is backend-whitelisted (verified in `iotUnits.ts`).
Sensor keys (`relay_N_state`) are intentionally distinct from actuator keys
(`relay_N`) — sensors register in `iot_sensors`, actuators in `iot_actuators`.

> The reported value is the **commanded** position (what the firmware set), not an
> independent sensed feedback. There is no aux-contact feedback wired on this board.

## Telemetry / state channels (don't conflate them)

- **`{topic_base}/telemetry`** — the 4 `relay_N_state` sensor readings, every
  `readingIntervalMinutes_` (default 5 min, portal-settable). Standard `message_id`
  dedup + store-and-forward.
- **`{topic_base}/state`** — actuator ACK (`{actuators:[{actuator_key,state}],
  reported_at}`), published on every command and (retained) on each connect so the
  backend's `current_state` reflects reality — including all-off at boot.
- **`{topic_base}/command`** — subscribed; backend → device `actuator_command`s.
- Status heartbeat every 60 s on `{topic_base}/status`.

## Self-test (wiring verification)

Build/flash `uvind_selftest` to cycle every relay in turn, run each buzzer's pulse
pattern, and blink the LEDs — confirm channel mapping + polarity **before**
onboarding. It's standalone (no core; `lib_ignore = BBHIotFirmwareCore`).

## Bootstrap defaults

- Setup-AP prefix: `UVIND-SETUP`
- Firmware version: `uv-indicator-1.0.0`
- Location hint: `UV System`
- Dev bootstrap broker: `10.24.16.105:1883` (DHCP-volatile — override in the setup
  portal if claims get no reply).

## Baseline deviations

- **Actuator device** — first consumer of the core actuator path (command subscribe
  + state ACK), which was previously deferred in CONTRACT.md.
- **Own status-LED scheme** — RED=power / GREEN=WiFi driven from the sketch; core
  red status LED disabled (`redLedPin = -1`), blue mapped to the onboard LED.

## Current status

Firmware scaffolded against the shared core (core actuator path implemented). Next:
build `uvind_app`, verify wiring with `uvind_selftest`, flash, onboard (the 6
actuators + 4 relay-state sensors auto-register on approval), then confirm a
command→ACK round-trip updates `current_state`.
