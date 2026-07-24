#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <MQTT.h>

// Chip-family differences (persistence, web server, WiFi, MAC, FS) are resolved
// here at compile time. This pulls in the right WiFi/WebServer/Preferences
// headers for the target, so include it before WiFiManager (which needs WiFi.h).
#include "platform/platform_compat.h"

#include <WiFiManager.h>

struct BbhSensorDefinition {
  const char* sensorKey;
  const char* sensorType;
  const char* unit;
};

struct BbhTelemetryReading {
  const char* sensorKey;
  float value;
  const char* unit;
  bool valid;
};

class BbhSensorAdapter {
 public:
  virtual ~BbhSensorAdapter() = default;
  virtual void begin() = 0;
  virtual void describeSensors(BbhSensorDefinition* definitions, size_t maxCount, size_t& outCount) = 0;
  virtual void readTelemetry(BbhTelemetryReading* readings, size_t maxCount, size_t& outCount) = 0;
};

// Declaration of one controllable output, sent in the bootstrap claim's
// `actuators[]` so the backend auto-registers it on approval (no manual Controls
// creation). `actuatorKey` is the id the firmware answers commands on and MUST be
// distinct from any feedback sensor_key; `actuatorName` is the human label; an
// unrecognised `actuatorType` falls back to "relay" backend-side (UI hint only).
struct BbhActuatorDefinition {
  const char* actuatorKey;
  const char* actuatorName;
  const char* actuatorType;
};

// Reported state of one controllable output, for the device -> backend ACK on
// `{topic_base}/state`. `state` is a stable string the backend stores verbatim as
// the actuator's current_state ("on"/"off"); point it at a string literal or
// long-lived buffer owned by the adapter.
struct BbhActuatorState {
  const char* actuatorKey;
  const char* state;
};

// Optional device outputs (relays, buzzers, indicator lights the BACKEND drives).
// Control is asymmetric per CONTRACT.md -> Actuator control: the backend publishes
// an `actuator_command` to `{topic_base}/command`, the device applies it and reports
// the resulting state on `{topic_base}/state`. Actuators are NOT declared in the
// bootstrap claim; an operator creates them in the Controls UI keyed by the same
// `actuatorKey` strings the firmware answers to (document them in PROJECT.md). A
// device with no outputs simply omits an adapter (the sensor-only constructor).
class BbhActuatorAdapter {
 public:
  virtual ~BbhActuatorAdapter() = default;
  virtual void begin() = 0;
  // Declare every controllable output for the bootstrap claim's `actuators[]`.
  // Set outCount to the number written.
  virtual void describeActuators(BbhActuatorDefinition* out, size_t maxCount, size_t& outCount) = 0;
  // Apply a command to `actuatorKey`. `command` is "on"/"off"/"set"; `value` is the
  // (possibly empty) command value, e.g. a level for "set". Return true if the key
  // matched a known actuator (so the core knows to ACK); false = unknown key.
  virtual bool applyCommand(const char* actuatorKey, const char* command, const char* value) = 0;
  // Fill the current state of every actuator (for the command ACK + the retained
  // state published on connect). Set outCount to the number written.
  virtual void reportState(BbhActuatorState* out, size_t maxCount, size_t& outCount) = 0;
  // Non-blocking render tick, called every loop() — drive time-based output
  // patterns here (e.g. a buzzer's on/off pulse cadence). MUST NOT block.
  virtual void tick() {}
};

struct BbhBootstrapDefaults {
  const char* brokerHost;
  uint16_t brokerPort;
  const char* username;
  const char* password;
  const char* clientId;
  const char* claimTopic;
  const char* replyPrefix;
  const char* locationHint;
};

struct BbhUiDefaults {
  const char* localUsername;
  const char* localPassword;
  const char* setupApPassword;
};

struct BbhDevicePins {
  int blueLedPin;
  int redLedPin;
  int resetButtonPin;
};

struct BbhTimingConfig {
  unsigned long telemetryIntervalMs;
  unsigned long statusIntervalMs;
  unsigned long wifiConnectTimeoutMs;
  unsigned long mqttRetryMs;
  unsigned long bootstrapClaimIntervalMs;
  unsigned long resetHoldMs;
  unsigned long redBlinkMs;
};

struct BbhFirmwareCoreConfig {
  const char* firmwareVersion;
  const char* setupApPrefix;
  BbhBootstrapDefaults bootstrap;
  BbhUiDefaults ui;
  BbhDevicePins pins;
  BbhTimingConfig timing;
};

class BbhIotFirmwareCore {
 public:
  // Sensor-only device (no controllable outputs) — unchanged from earlier projects.
  BbhIotFirmwareCore(const BbhFirmwareCoreConfig& config, BbhSensorAdapter& sensorAdapter)
      : BbhIotFirmwareCore(config, sensorAdapter, nullptr) {}
  // Device with actuators (relays/buzzers/lights the backend controls). See
  // BbhActuatorAdapter; Project6-UV-Indicator is the first consumer.
  BbhIotFirmwareCore(const BbhFirmwareCoreConfig& config, BbhSensorAdapter& sensorAdapter,
                     BbhActuatorAdapter& actuatorAdapter)
      : BbhIotFirmwareCore(config, sensorAdapter, &actuatorAdapter) {}

  void setup();
  void loop();

 private:
  // Designated constructor; a null actuatorAdapter means a sensor-only device.
  BbhIotFirmwareCore(const BbhFirmwareCoreConfig& config, BbhSensorAdapter& sensorAdapter,
                     BbhActuatorAdapter* actuatorAdapter);

  static constexpr size_t kLogCapacity = 50;
  static constexpr size_t kMaxSensors = 8;
  static constexpr size_t kMaxActuators = 8;
  static constexpr size_t kHostMax = 80;
  static constexpr size_t kUserMax = 64;
  static constexpr size_t kPassMax = 80;
  static constexpr size_t kTopicMax = 120;
  static constexpr size_t kClientIdMax = 80;
  static constexpr size_t kDeviceKeyMax = 64;
  static constexpr size_t kDeviceNameMax = 64;
  static constexpr size_t kDeviceTypeMax = 32;
  static constexpr size_t kLocationCodeMax = 32;
  static constexpr size_t kLocationHintMax = 64;
  static constexpr size_t kProtocolMax = 16;
  static constexpr size_t kSensorJsonMax = 1024;
  static constexpr size_t kTelemetrySlotSize = 512;
  static constexpr uint16_t kTelemetryBufferCapacity = 512;
  static constexpr uint16_t kDefaultReadingMinutes = 5;
  static constexpr uint16_t kMaxReadingMinutes = 1440;  // 24h ceiling
  // Consecutive runtime MQTT auth rejections tolerated before the device
  // assumes its credentials were revoked and falls back to bootstrap/claim.
  // A single rejection is treated as transient (e.g. broker mid-rotation),
  // so a credential rotation no longer instantly de-registers the device.
  static constexpr uint8_t kMaxRuntimeAuthFailures = 5;

  enum DeviceMode {
    DEVICE_MODE_BOOTSTRAP,
    DEVICE_MODE_RUNTIME
  };

  struct StoredSensorDefinition {
    char sensorKey[32];
    char sensorType[32];
    char unit[16];
  };

  static BbhIotFirmwareCore* activeInstance;
  static void dispatchMqttMessage(String& topic, String& payload);

  void onMqttMessage(String& topic, String& payload);
  void initializePins();
  void initializeSensorDefinitions();
  void loadRuntimeConfig();
  void loadAndIncrementBootSeq();
  void persistBootstrapConfig();
  void persistFinalConfig();
  void resetFinalConfigInMemory();
  void clearFinalConfig();
  bool hasFinalConfig() const;
  bool connectWifi();
  void applyWifiPerformanceProfile();
  bool runOnboardingPortal(bool forcePortal);
  bool connectBootstrapMqtt();
  bool connectRuntimeMqtt();
  void performPendingMqttReconnect();
  void publishBootstrapClaim();
  bool applyApprovedConfig(JsonDocument& doc);
  void handleRuntimeConfigMessage(const String& payload);
  void handleActuatorCommand(const String& payload);
  void publishActuatorState(bool retain);
  void publishConfigAck(const String& rotationId);
  void publishRuntimeStatus(const char* state);
  bool buildTelemetryRecord(String& outJson);
  bool publishTelemetryRecord(const String& json);
  void drainTelemetryBuffer(size_t maxRecords);
  void initTelemetryBuffer();
  bool telemetryBufferEnqueue(const String& json);
  bool telemetryBufferPeek(String& outJson);
  void telemetryBufferPopFront();
  void persistTelemetryBufferMeta();
  void syncTimeIfNeeded();
  bool isTimeValid();
  String currentIso8601();
  void updateRuntimeOfflineWill();
  void ensureLocalServer();
  void clearProvisioningAndRestart();
  void handleFactoryResetButton();
  void handleLocalStatusPage();
  void handleLocalStatusJson();
  void handleLocalSetupSave();
  void handleLocalRebootstrap();
  bool requireLocalAuth();
  void requestMqttReconnect(const String& reason);
  void logEvent(const String& message);
  void setBlueLed(bool on);
  void setRedLed(bool on);
  void captureLatestReadings();

  String hardwareId() const;
  String hardwareIdCompact() const;
  String bootstrapReplyTopic() const;
  String bootstrapClientId() const;
  String sanitizeMqttToken(const String& value) const;
  void waitForBootstrapReply(unsigned long timeoutMs);
  String formatUptimeDhM(unsigned long ms) const;
  String htmlEscape(const String& input) const;
  String buildSetupApName() const;

  static void copyTrimmed(char* target, size_t targetSize, const char* source);
  static bool isBlank(const char* value);

  const BbhFirmwareCoreConfig& config_;
  BbhSensorAdapter& sensorAdapter_;
  BbhActuatorAdapter* actuatorAdapter_;  // null for sensor-only devices

  BbhPreferences configStore_;
  BbhWebServer localServer_;
  WiFiClient net_;
  // Read/write buffer sizes are set in the constructor init list — mqtt_(read,
  // write) — NOT here (an init-list entry overrides any default member init).
  // Read must fit an ENTIRE incoming packet: the retained bootstrap
  // `device_config` runs ~1.1 KB for a 4-sensor device, so the read buffer is
  // sized well above that. Writes stream their payload after a short header, so
  // the write buffer only needs header+topic room.
  MQTTClient mqtt_;

  DeviceMode deviceMode_;
  bool localServerStarted_;
  bool resetPressActive_;
  bool resetLedOn_;
  bool resetNotified_;
  bool rebootRequested_;
  bool bootstrapClaimPublished_;
  bool bootstrapClaimPending_;
  bool mqttReconnectRequested_;
  unsigned long resetPressStartMs_;
  unsigned long resetLastBlinkMs_;
  unsigned long lastTelemetryAt_;
  unsigned long lastStatusAt_;
  unsigned long lastMqttAttemptAt_;
  unsigned long lastBootstrapClaimAt_;
  uint32_t runtimeMessageCounter_;
  uint8_t runtimeAuthFailures_;
  uint32_t bootSeq_;
  String lastPublishSummary_;
  String lastBootstrapStatus_;
  String lastClaimId_;
  String approvedAt_;
  String eventLog_[kLogCapacity];
  size_t eventLogHead_;
  size_t eventLogCount_;

  char bootstrapBrokerHost_[kHostMax];
  char bootstrapUsername_[kUserMax];
  char bootstrapPassword_[kPassMax];
  char bootstrapClientId_[kClientIdMax];
  char bootstrapClaimTopic_[kTopicMax];
  char bootstrapReplyPrefix_[kTopicMax];
  char locationHint_[kLocationHintMax];
  uint16_t bootstrapBrokerPort_;

  char deviceKey_[kDeviceKeyMax];
  char deviceName_[kDeviceNameMax];
  char deviceType_[kDeviceTypeMax];
  char locationCode_[kLocationCodeMax];
  char runtimeBrokerHost_[kHostMax];
  char runtimeProtocol_[kProtocolMax];
  char runtimeUsername_[kUserMax];
  char runtimePassword_[kPassMax];
  char runtimeClientId_[kClientIdMax];
  char runtimeTopicBase_[kTopicMax];
  char runtimeStatusTopic_[kTopicMax];
  char runtimeTelemetryTopic_[kTopicMax];
  uint16_t runtimeBrokerPort_;
  uint16_t runtimeQos_;
  uint16_t runtimeKeepaliveSeconds_;
  uint16_t expectedIntervalMinutes_;
  uint16_t offlineAfterMinutes_;
  uint16_t readingIntervalMinutes_;  // user-configurable telemetry cadence
  uint8_t wifiTxPower_;  // WiFi TX power in dBm (5-20, default 20 = max)
  StoredSensorDefinition sensorDefs_[kMaxSensors];
  size_t sensorDefCount_;
  BbhTelemetryReading latestReadings_[kMaxSensors];
  size_t latestReadingCount_;
  bool fsReady_;
  bool timeValid_;
  bool ntpConfigured_;
  uint16_t tbHead_;
  uint16_t tbCount_;
};
