#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <MQTT.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
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
  BbhIotFirmwareCore(const BbhFirmwareCoreConfig& config, BbhSensorAdapter& sensorAdapter);

  void setup();
  void loop();

 private:
  static constexpr size_t kLogCapacity = 50;
  static constexpr size_t kMaxSensors = 8;
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
  bool runOnboardingPortal(bool forcePortal);
  bool connectBootstrapMqtt();
  bool connectRuntimeMqtt();
  void performPendingMqttReconnect();
  void publishBootstrapClaim();
  bool applyApprovedConfig(JsonDocument& doc);
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

  Preferences configStore_;
  WebServer localServer_;
  WiFiClient net_;
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
