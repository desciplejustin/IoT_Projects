// Project6 — UV Indicator
//
// An ANNUNCIATOR / actuator panel on an ESP32 WROOM-32. Unlike the telemetry
// nodes, its job is to DRIVE outputs the backend controls, over the shared BBH
// wire contract (CONTRACT.md -> Actuator control): the backend publishes an
// `actuator_command` to {topic_base}/command, the firmware applies it and ACKs
// the resulting state on {topic_base}/state. This is the first device to use the
// core's actuator path (BbhActuatorAdapter).
//
// Outputs:
//   4x relay   (relay_1..relay_4)  — on/off when commanded
//   2x buzzer  (buzzer_1, buzzer_2) — pulse while commanded ON:
//        buzzer_1 = 1.0 s on / 1.0 s off   (slow)
//        buzzer_2 = 0.5 s on / 0.5 s off   (fast)
//   2x status LED (external, driven locally, NOT backend-controlled):
//        RED   = power OK   (lit whenever the MCU is powered and running)
//        GREEN = WiFi link  (lit while associated to WiFi)
//
// Onboarding: an actuator-only device is rejected by the claim (it needs >=1
// canonical sensor, and actuators are not declared in the claim — an operator
// adds them in the Controls UI keyed by the actuator_key strings below). So the
// board also reports each relay's position as a discrete `state` sensor
// (relay_1_state..relay_4_state) — an honest boolean that both satisfies
// onboarding and gives relay-position history. See PROJECT.md.

#include <Arduino.h>

#ifndef SELFTEST_ONLY
#define SELFTEST_ONLY 0
#endif

// ---------------------------------------------------------------------------
// Output pin map  (ESP32 WROOM-32 / esp32dev — raw GPIO numbers)
// ---------------------------------------------------------------------------
// Chosen to avoid strapping pins (0,2,12,15), the flash pins (6-11), the
// input-only pins (34-39) and UART0 (1,3, used for the serial log). Wire your
// relay module / buzzers / LEDs to these, or edit the map to match fixed hardware.
// This devkit's silkscreen labels pins "D<n>" where n == the GPIO number, so
// GPIO16 = D16, GPIO25 = D25, etc. See PROJECT.md for the full labelled map.
struct RelayCh {
  const char* key;
  uint8_t pin;
};
static const RelayCh kRelays[] = {
    {"relay_1", 16},
    {"relay_2", 17},
    {"relay_3", 18},
    {"relay_4", 19},
};
static const size_t kRelayCount = sizeof(kRelays) / sizeof(kRelays[0]);

struct BuzzerCh {
  const char* key;
  uint8_t pin;
  unsigned long halfPeriodMs;  // on-time == off-time
};
static const BuzzerCh kBuzzers[] = {
    {"buzzer_1", 25, 1000},  // 1.0 s on / 1.0 s off
    {"buzzer_2", 26, 500},   // 0.5 s on / 0.5 s off
};
static const size_t kBuzzerCount = sizeof(kBuzzers) / sizeof(kBuzzers[0]);

// External status LEDs (driven locally, see file header).
static const uint8_t LED_RED_POWER = 23;   // lit = powered/running
static const uint8_t LED_GREEN_WIFI = 22;  // lit = WiFi associated

// ---- Output polarity ------------------------------------------------------
// Most opto-isolated relay boards are ACTIVE-LOW (the channel energises when the
// GPIO is driven LOW). Buzzers wired to a transistor/driver are usually
// ACTIVE-HIGH. If a relay clicks on at boot or the on/off sense is inverted,
// flip RELAY_ACTIVE_LOW; likewise BUZZER_ACTIVE_LOW for the buzzers.
static const bool RELAY_ACTIVE_LOW = true;
static const bool BUZZER_ACTIVE_LOW = false;

static inline void driveRelay(uint8_t pin, bool on) {
  digitalWrite(pin, (RELAY_ACTIVE_LOW ? !on : on) ? HIGH : LOW);
}
static inline void driveBuzzer(uint8_t pin, bool on) {
  digitalWrite(pin, (BUZZER_ACTIVE_LOW ? !on : on) ? HIGH : LOW);
}

// ---------------------------------------------------------------------------
// IndicatorOutputs — owns all output state + rendering. Plain class (no core
// dependency) so it is reused by both the self-test env and the full firmware.
// tick() must be called every loop() (non-blocking); it renders the buzzer pulse
// cadence while a buzzer is commanded ON.
// ---------------------------------------------------------------------------
class IndicatorOutputs {
 public:
  void begin() {
    for (size_t i = 0; i < kRelayCount; i++) {
      relayOn_[i] = false;
      pinMode(kRelays[i].pin, OUTPUT);
      driveRelay(kRelays[i].pin, false);  // boot-safe: relays OFF
    }
    for (size_t i = 0; i < kBuzzerCount; i++) {
      pinMode(kBuzzers[i].pin, OUTPUT);
      setBuzzer(i, false);  // boot-safe: buzzers silent
    }
  }

  void setRelay(size_t i, bool on) {
    if (i >= kRelayCount) return;
    relayOn_[i] = on;
    driveRelay(kRelays[i].pin, on);
  }

  void setBuzzer(size_t i, bool on) {
    if (i >= kBuzzerCount) return;
    buzzerOn_[i] = on;
    buzzerPhase_[i] = on;  // start ON immediately when triggered
    buzzerToggleAt_[i] = millis();
    driveBuzzer(kBuzzers[i].pin, on);
  }

  // Non-blocking: toggle each commanded-ON buzzer at its half-period cadence.
  void tick() {
    unsigned long now = millis();
    for (size_t i = 0; i < kBuzzerCount; i++) {
      if (!buzzerOn_[i]) continue;
      if (now - buzzerToggleAt_[i] >= kBuzzers[i].halfPeriodMs) {
        buzzerPhase_[i] = !buzzerPhase_[i];
        driveBuzzer(kBuzzers[i].pin, buzzerPhase_[i]);
        buzzerToggleAt_[i] = now;
      }
    }
  }

  bool relayOn(size_t i) const { return i < kRelayCount ? relayOn_[i] : false; }
  bool buzzerOn(size_t i) const { return i < kBuzzerCount ? buzzerOn_[i] : false; }

 private:
  bool relayOn_[kRelayCount] = {false};
  bool buzzerOn_[kBuzzerCount] = {false};
  bool buzzerPhase_[kBuzzerCount] = {false};
  unsigned long buzzerToggleAt_[kBuzzerCount] = {0};
};

IndicatorOutputs outputs;

#if SELFTEST_ONLY

// Wiring-verification mode: cycles every output so an installer can confirm each
// relay channel, buzzer and LED is wired correctly BEFORE onboarding the device.
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("Project6 UV Indicator - output self-test");
  Serial.println("Cycles relays 1..4, pulses each buzzer, blinks LEDs.");
  pinMode(LED_RED_POWER, OUTPUT);
  pinMode(LED_GREEN_WIFI, OUTPUT);
  digitalWrite(LED_RED_POWER, HIGH);  // power LED on for the duration
  outputs.begin();
}

void loop() {
  // Relays: energise each in turn for 700 ms.
  for (size_t i = 0; i < kRelayCount; i++) {
    Serial.printf("%s (GPIO%u) ON\n", kRelays[i].key, kRelays[i].pin);
    outputs.setRelay(i, true);
    digitalWrite(LED_GREEN_WIFI, HIGH);
    delay(700);
    outputs.setRelay(i, false);
    digitalWrite(LED_GREEN_WIFI, LOW);
    delay(300);
  }

  // Buzzers: run each pulse pattern for ~3 s so the cadence is audible.
  for (size_t i = 0; i < kBuzzerCount; i++) {
    Serial.printf("%s (GPIO%u) pulsing %lums on/off\n", kBuzzers[i].key, kBuzzers[i].pin,
                  kBuzzers[i].halfPeriodMs);
    outputs.setBuzzer(i, true);
    unsigned long until = millis() + 3000;
    while (millis() < until) {
      outputs.tick();
      delay(5);
    }
    outputs.setBuzzer(i, false);
    delay(300);
  }
  Serial.println("--- cycle complete ---");
}

#else

#include <BBHIotFirmwareCore.h>

#ifndef BBH_BOOTSTRAP_USERNAME
#define BBH_BOOTSTRAP_USERNAME ""
#endif
#ifndef BBH_BOOTSTRAP_PASSWORD
#define BBH_BOOTSTRAP_PASSWORD ""
#endif
#ifndef BBH_BOOTSTRAP_CLIENT_ID
#define BBH_BOOTSTRAP_CLIENT_ID "bbh-iot-bootstrap"
#endif

// Local diagnostics dashboard + setup-AP credentials. Real values are supplied
// per build via platformio_override.ini (gitignored); these committed defaults
// are non-secret placeholders and must be overridden before deployment.
#ifndef BBH_LOCAL_USERNAME
#define BBH_LOCAL_USERNAME "admin"
#endif
#ifndef BBH_LOCAL_PASSWORD
#define BBH_LOCAL_PASSWORD "changeme"
#endif
#ifndef BBH_SETUP_AP_PASSWORD
#define BBH_SETUP_AP_PASSWORD "bbh-setup"
#endif

// Decode a command into a desired ON/OFF. `on`/`off` are the norm; `set` treats a
// truthy value as ON so a generic switch control still works.
static bool commandWantsOn(const char* command, const char* value) {
  if (strcmp(command, "on") == 0) return true;
  if (strcmp(command, "off") == 0) return false;
  if (strcmp(command, "set") == 0) {
    return value != nullptr &&
           (strcmp(value, "on") == 0 || strcmp(value, "1") == 0 || strcmp(value, "true") == 0);
  }
  return false;  // unknown command -> treat as OFF
}

// Default human labels for the claim's actuators[] (operator can rename in-app).
static const char* const kRelayNames[kRelayCount] = {"Relay 1", "Relay 2", "Relay 3",
                                                     "Relay 4"};
static const char* const kBuzzerNames[kBuzzerCount] = {"Buzzer 1 (1s pulse)",
                                                       "Buzzer 2 (0.5s pulse)"};

// Actuator adapter: declares the 4 relays + 2 buzzers in the claim, maps backend
// commands to them, and reports current state back for the ACK. Actuators
// auto-register on approval; the operator only needs to rename/arrange them.
class IndicatorActuatorAdapter : public BbhActuatorAdapter {
 public:
  void begin() override {
    outputs.begin();
    Serial.printf("Indicator outputs: %u relays + %u buzzers\n", (unsigned)kRelayCount,
                  (unsigned)kBuzzerCount);
  }

  // Declared in the claim's actuators[] so the backend auto-registers all 6 outputs
  // on approval. actuator_name is a sensible default the operator can rename; the
  // "buzzer" type is a UI hint (backend falls back to "relay" if unrecognised).
  void describeActuators(BbhActuatorDefinition* out, size_t maxCount, size_t& outCount) override {
    outCount = 0;
    for (size_t i = 0; i < kRelayCount && outCount < maxCount; i++) {
      out[outCount++] = {kRelays[i].key, kRelayNames[i], "relay"};
    }
    for (size_t i = 0; i < kBuzzerCount && outCount < maxCount; i++) {
      out[outCount++] = {kBuzzers[i].key, kBuzzerNames[i], "buzzer"};
    }
  }

  bool applyCommand(const char* actuatorKey, const char* command, const char* value) override {
    bool on = commandWantsOn(command, value);
    for (size_t i = 0; i < kRelayCount; i++) {
      if (strcmp(actuatorKey, kRelays[i].key) == 0) {
        outputs.setRelay(i, on);
        return true;
      }
    }
    for (size_t i = 0; i < kBuzzerCount; i++) {
      if (strcmp(actuatorKey, kBuzzers[i].key) == 0) {
        outputs.setBuzzer(i, on);
        return true;
      }
    }
    return false;  // unknown key
  }

  void reportState(BbhActuatorState* out, size_t maxCount, size_t& outCount) override {
    outCount = 0;
    for (size_t i = 0; i < kRelayCount && outCount < maxCount; i++) {
      out[outCount++] = {kRelays[i].key, outputs.relayOn(i) ? "on" : "off"};
    }
    for (size_t i = 0; i < kBuzzerCount && outCount < maxCount; i++) {
      out[outCount++] = {kBuzzers[i].key, outputs.buzzerOn(i) ? "on" : "off"};
    }
  }

  void tick() override { outputs.tick(); }
};

// Sensor adapter: exposes each relay's commanded position as a discrete `state`
// sensor (1 = energised). Satisfies onboarding AND gives relay-position history.
// The outputs are initialised by the actuator adapter's begin() (called after
// this one), so readTelemetry() always sees a valid state.
class RelayStateSensorAdapter : public BbhSensorAdapter {
 public:
  void begin() override {}

  void describeSensors(BbhSensorDefinition* definitions, size_t maxCount, size_t& outCount) override {
    outCount = 0;
    for (size_t i = 0; i < kRelayCount && outCount < maxCount; i++) {
      definitions[outCount++] = {kRelayStateKeys[i], "relay_feedback", "state"};
    }
  }

  void readTelemetry(BbhTelemetryReading* readings, size_t maxCount, size_t& outCount) override {
    outCount = 0;
    for (size_t i = 0; i < kRelayCount && outCount < maxCount; i++) {
      float value = outputs.relayOn(i) ? 1.0f : 0.0f;
      readings[outCount++] = {kRelayStateKeys[i], value, "state", true};
    }
  }

 private:
  // Distinct keys from the actuator keys: sensors register in iot_sensors,
  // actuators in iot_actuators — keeping them separate avoids any collision.
  static constexpr const char* kRelayStateKeys[4] = {
      "relay_1_state", "relay_2_state", "relay_3_state", "relay_4_state"};
};
constexpr const char* RelayStateSensorAdapter::kRelayStateKeys[4];

const BbhFirmwareCoreConfig kFirmwareConfig = {
    "uv-indicator-1.0.0",
    "UVIND-SETUP",
    {
        BBH_BOOTSTRAP_BROKER_HOST,
        BBH_BOOTSTRAP_BROKER_PORT,
        BBH_BOOTSTRAP_USERNAME,
        BBH_BOOTSTRAP_PASSWORD,
        BBH_BOOTSTRAP_CLIENT_ID,
        BBH_BOOTSTRAP_CLAIM_TOPIC,
        BBH_BOOTSTRAP_REPLY_PREFIX,
        "UV System",
    },
    {
        BBH_LOCAL_USERNAME,
        BBH_LOCAL_PASSWORD,
        BBH_SETUP_AP_PASSWORD,
    },
    {
        // This device drives its OWN external status LEDs (RED=power, GREEN=WiFi)
        // from the sketch, so the core's status-LED scheme is mostly disabled:
        //   blueLedPin  = GPIO2 (onboard LED) -> core health blink, handy diagnostic
        //   redLedPin   = -1 (disabled; would clash with our RED=power LED meaning)
        //   resetButtonPin = GPIO14, momentary to GND (safe, non-strapping, not
        //     affected by the GPIO0/DTR accidental-reset trap).
        2,   // blueLedPin (onboard)
        -1,  // redLedPin (disabled)
        14,  // resetButtonPin
    },
    {
        // NOTE: telemetryIntervalMs is UNUSED by the core — real cadence is the
        // portal "Reading interval (minutes)" (default 5). Left to fill the struct.
        60000UL,  // telemetryIntervalMs (unused)
        60000UL,  // statusIntervalMs
        20000UL,  // wifiConnectTimeoutMs
        5000UL,   // mqttRetryMs
        30000UL,  // bootstrapClaimIntervalMs
        5000UL,   // resetHoldMs (long-press to factory reset)
        250UL,    // redBlinkMs
    },
};

RelayStateSensorAdapter sensorAdapter;
IndicatorActuatorAdapter actuatorAdapter;
BbhIotFirmwareCore firmwareCore(kFirmwareConfig, sensorAdapter, actuatorAdapter);

// External status LEDs — driven here, not by the core (see header).
static void updateStatusLeds() {
  digitalWrite(LED_GREEN_WIFI, WiFi.status() == WL_CONNECTED ? HIGH : LOW);
}

void setup() {
  firmwareCore.setup();
  pinMode(LED_RED_POWER, OUTPUT);
  pinMode(LED_GREEN_WIFI, OUTPUT);
  digitalWrite(LED_RED_POWER, HIGH);  // power OK — on whenever the board is running
  updateStatusLeds();
}

void loop() {
  firmwareCore.loop();  // also ticks the actuator adapter (buzzer pulse cadence)
  updateStatusLeds();
}

#endif
