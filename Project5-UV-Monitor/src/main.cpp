// Project5 — UV Monitor
//
// Monitors the open/closed state of 4 contactors via dry auxiliary contacts on
// an ESP8266 NodeMCU. Each input is a discrete `state` sensor on the shared BBH
// wire contract (value 1 = closed/energized, 0 = open) and is reported at the
// telemetry interval like any other sensor. See PROJECT.md for wiring/polarity.
//
// Inputs are DEBOUNCED continuously: the board samples every loop, holds the
// stable (settled) state per input, and readTelemetry() returns that held state
// — so contact bounce / contactor chatter never produces a spurious reading.

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Contactor input map  (NodeMCU / ESP8266 pin labels)
// ---------------------------------------------------------------------------
// Requested inputs were D1, D2, D3, D5. D3 = GPIO0 is a BOOT-STRAPPING pin: a
// closed contactor pulling it LOW at power-up drops the ESP8266 into flash mode
// and it will not boot. The 4th input is therefore moved off D3 to D6 (GPIO12),
// which behaves identically at runtime with no boot hazard. GPIO0 (D3) is reused
// for the factory-reset button (the onboard FLASH button is already wired there).
//
//   Input        NodeMCU   GPIO   strapping?
//   contactor_1   D1        5      no
//   contactor_2   D2        4      no
//   contactor_3   D5        14     no
//   contactor_4   D6        12     no   (moved off D3/GPIO0 — see above)
//
// Wiring: each aux contact connects its GPIO to GND. Inputs use INPUT_PULLUP, so
// an OPEN contact reads HIGH and a CLOSED contact reads LOW. We report value=1
// for CLOSED. If a given contactor's aux is normally-closed (NC), set invert=true
// for that channel so "energized" still maps to 1.
struct ContactorInput {
  const char* sensorKey;
  uint8_t pin;
  bool invert;  // true if the aux contact is normally-closed (NC)
};

static const ContactorInput kInputs[] = {
    {"contactor_1", 5, false},   // D1
    {"contactor_2", 4, false},   // D2
    {"contactor_3", 14, false},  // D5
    {"contactor_4", 12, false},  // D6 (was D3/GPIO0)
};
static const size_t kInputCount = sizeof(kInputs) / sizeof(kInputs[0]);

// Contacts must read steady for this long before the held state changes. 50 ms
// comfortably covers relay/contactor contact bounce; raise it if a noisy
// contactor still chatters.
static const unsigned long kDebounceMs = 50;

#ifndef SENSOR_ID_SCAN_ONLY
#define SENSOR_ID_SCAN_ONLY 0
#endif

// ---------------------------------------------------------------------------
// Debounced contactor sampler — shared by scan mode and full firmware.
// poll() must be called every loop() iteration (it is non-blocking).
// ---------------------------------------------------------------------------
class ContactorSampler {
 public:
  void begin() {
    for (size_t i = 0; i < kInputCount; i++) {
      pinMode(kInputs[i].pin, INPUT_PULLUP);
      int level = digitalRead(kInputs[i].pin);
      rawLevel_[i] = level;
      stableLevel_[i] = level;
      lastChangeMs_[i] = millis();
    }
  }

  void poll() {
    unsigned long now = millis();
    for (size_t i = 0; i < kInputCount; i++) {
      int level = digitalRead(kInputs[i].pin);
      if (level != rawLevel_[i]) {
        rawLevel_[i] = level;
        lastChangeMs_[i] = now;
      }
      if (stableLevel_[i] != level && (now - lastChangeMs_[i]) >= kDebounceMs) {
        stableLevel_[i] = level;
      }
    }
  }

  // Held/settled logical state: true = contactor CLOSED (energized).
  bool closed(size_t i) const {
    bool isClosed = (stableLevel_[i] == LOW);  // INPUT_PULLUP: closed pulls to GND
    return kInputs[i].invert ? !isClosed : isClosed;
  }

 private:
  int rawLevel_[kInputCount];
  int stableLevel_[kInputCount];
  unsigned long lastChangeMs_[kInputCount];
};

ContactorSampler contactors;

#if SENSOR_ID_SCAN_ONLY

// Wiring-verification mode: continuously print raw + debounced state for each
// input so you can confirm each contactor lands on the right channel and the
// polarity is correct BEFORE onboarding the device.
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("Project5 UV Monitor — contactor input scan");
  Serial.println("value: 1 = closed/energized, 0 = open");
  contactors.begin();
}

void loop() {
  contactors.poll();
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint >= 500) {
    lastPrint = millis();
    for (size_t i = 0; i < kInputCount; i++) {
      Serial.printf("%-12s GPIO%-2u  raw=%s  state=%u\n",
                    kInputs[i].sensorKey, kInputs[i].pin,
                    digitalRead(kInputs[i].pin) == LOW ? "LOW " : "HIGH",
                    contactors.closed(i) ? 1u : 0u);
    }
    Serial.println("---");
  }
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

// Adapter: exposes the 4 contactors as discrete `state` sensors. The debounced
// state is maintained by the global `contactors` sampler (polled in loop());
// readTelemetry() just snapshots the currently-held state, so the value sent at
// each interval is always the stable one.
class ContactorAdapter : public BbhSensorAdapter {
 public:
  void begin() override {
    contactors.begin();
    Serial.printf("Contactor monitor: %u inputs\n", (unsigned)kInputCount);
  }

  void describeSensors(BbhSensorDefinition* definitions, size_t maxCount, size_t& outCount) override {
    outCount = 0;
    for (size_t i = 0; i < kInputCount && outCount < maxCount; i++) {
      definitions[outCount++] = {kInputs[i].sensorKey, "contactor", "state"};
    }
  }

  void readTelemetry(BbhTelemetryReading* readings, size_t maxCount, size_t& outCount) override {
    outCount = 0;
    for (size_t i = 0; i < kInputCount && outCount < maxCount; i++) {
      float value = contactors.closed(i) ? 1.0f : 0.0f;
      readings[outCount++] = {kInputs[i].sensorKey, value, "state", true};
    }
  }
};

const BbhFirmwareCoreConfig kFirmwareConfig = {
    "uv-monitor-1.0.0",
    "UVMON-SETUP",
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
        // NodeMCU pins. Status LEDs on free non-strapping GPIOs (wire discrete
        // LEDs; the onboard LED on GPIO2 is a strapping pin and active-low, so
        // it is intentionally not used). Factory-reset button on the onboard
        // FLASH button (GPIO0 / D3, active-low) — only pressed at runtime, so
        // its boot-strapping role is not a concern.
        13,  // blueLedPin  (D7)
        16,  // redLedPin   (D0)
        0,   // resetButtonPin (D3 / FLASH)
    },
    {
        // NOTE: telemetryIntervalMs is currently UNUSED by the core — the real
        // telemetry cadence is the portal's "Reading interval (minutes)"
        // (readingIntervalMinutes_, default 5, whole minutes). Left here to fill
        // the struct.
        60000UL,  // telemetryIntervalMs (unused)
        60000UL,  // statusIntervalMs
        20000UL,  // wifiConnectTimeoutMs
        5000UL,   // mqttRetryMs
        30000UL,  // bootstrapClaimIntervalMs
        5000UL,   // resetHoldMs (long-press to factory reset)
        250UL,    // redBlinkMs
    },
};

ContactorAdapter sensorAdapter;
BbhIotFirmwareCore firmwareCore(kFirmwareConfig, sensorAdapter);

void setup() {
  firmwareCore.setup();
}

void loop() {
  contactors.poll();   // continuous, non-blocking debounce
  firmwareCore.loop();
}

#endif
