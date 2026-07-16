#include "BBHIotFirmwareCore.h"

#include <LittleFS.h>
#include <time.h>

// Opt-in WiFi performance profile. When enabled, the radio runs at maximum TX
// power with modem sleep disabled: stronger uplink and steadier connectivity at
// the cost of higher idle current. Off by default; enable per project for
// mains-powered devices with -D BBH_WIFI_HIGH_PERFORMANCE=1 in platformio.ini.
#ifndef BBH_WIFI_HIGH_PERFORMANCE
#define BBH_WIFI_HIGH_PERFORMANCE 0
#endif

namespace {
constexpr const char* kTelemetryBufferPath = "/tbuf.bin";
constexpr time_t kMinValidEpoch = 1700000000;  // ~2023-11, sanity floor for NTP sync
}  // namespace

BbhIotFirmwareCore* BbhIotFirmwareCore::activeInstance = nullptr;

namespace {

const char* mqttErrorName(lwmqtt_err_t error) {
  switch (error) {
    case LWMQTT_SUCCESS:
      return "success";
    case LWMQTT_BUFFER_TOO_SHORT:
      return "buffer_too_short";
    case LWMQTT_VARNUM_OVERFLOW:
      return "varnum_overflow";
    case LWMQTT_NETWORK_FAILED_CONNECT:
      return "network_failed_connect";
    case LWMQTT_NETWORK_TIMEOUT:
      return "network_timeout";
    case LWMQTT_NETWORK_FAILED_READ:
      return "network_failed_read";
    case LWMQTT_NETWORK_FAILED_WRITE:
      return "network_failed_write";
    case LWMQTT_REMAINING_LENGTH_OVERFLOW:
      return "remaining_length_overflow";
    case LWMQTT_REMAINING_LENGTH_MISMATCH:
      return "remaining_length_mismatch";
    case LWMQTT_MISSING_OR_WRONG_PACKET:
      return "missing_or_wrong_packet";
    case LWMQTT_CONNECTION_DENIED:
      return "connection_denied";
    case LWMQTT_FAILED_SUBSCRIPTION:
      return "failed_subscription";
    case LWMQTT_SUBACK_ARRAY_OVERFLOW:
      return "suback_array_overflow";
    case LWMQTT_PONG_TIMEOUT:
      return "pong_timeout";
    default:
      return "unknown_error";
  }
}

const char* mqttReturnCodeName(lwmqtt_return_code_t code) {
  switch (code) {
    case LWMQTT_CONNECTION_ACCEPTED:
      return "accepted";
    case LWMQTT_UNACCEPTABLE_PROTOCOL:
      return "unacceptable_protocol";
    case LWMQTT_IDENTIFIER_REJECTED:
      return "identifier_rejected";
    case LWMQTT_SERVER_UNAVAILABLE:
      return "server_unavailable";
    case LWMQTT_BAD_USERNAME_OR_PASSWORD:
      return "bad_username_or_password";
    case LWMQTT_NOT_AUTHORIZED:
      return "not_authorized";
    case LWMQTT_UNKNOWN_RETURN_CODE:
      return "unknown_return_code";
    default:
      return "unexpected_return_code";
  }
}

String mqttFailureSummary(MQTTClient& client) {
  String summary = "err=";
  summary += mqttErrorName(client.lastError());
  summary += " (";
  summary += static_cast<int>(client.lastError());
  summary += "), rc=";
  summary += mqttReturnCodeName(client.returnCode());
  summary += " (";
  summary += static_cast<int>(client.returnCode());
  summary += ")";
  return summary;
}

bool isRuntimeAuthDenied(MQTTClient& client) {
  return client.returnCode() == LWMQTT_BAD_USERNAME_OR_PASSWORD ||
         client.returnCode() == LWMQTT_NOT_AUTHORIZED;
}

}  // namespace

BbhIotFirmwareCore::BbhIotFirmwareCore(
    const BbhFirmwareCoreConfig& config,
    BbhSensorAdapter& sensorAdapter)
    : config_(config),
      sensorAdapter_(sensorAdapter),
      localServer_(80),
      mqtt_(1024, 1024),
      deviceMode_(DEVICE_MODE_BOOTSTRAP),
      localServerStarted_(false),
      resetPressActive_(false),
      resetLedOn_(false),
      resetNotified_(false),
      rebootRequested_(false),
      bootstrapClaimPublished_(false),
      bootstrapClaimPending_(false),
      mqttReconnectRequested_(false),
      resetPressStartMs_(0),
      resetLastBlinkMs_(0),
      lastTelemetryAt_(0),
      lastStatusAt_(0),
      lastMqttAttemptAt_(0),
      lastBootstrapClaimAt_(0),
      runtimeMessageCounter_(0),
      runtimeAuthFailures_(0),
      bootSeq_(0),
      lastPublishSummary_("none"),
      lastBootstrapStatus_("waiting"),
      lastClaimId_(""),
      approvedAt_(""),
      eventLogHead_(0),
      eventLogCount_(0),
      bootstrapBrokerPort_(config.bootstrap.brokerPort),
      runtimeBrokerPort_(0),
      runtimeQos_(1),
      runtimeKeepaliveSeconds_(60),
      expectedIntervalMinutes_(0),
      offlineAfterMinutes_(0),
      readingIntervalMinutes_(kDefaultReadingMinutes),
      sensorDefCount_(0),
      latestReadingCount_(0),
      fsReady_(false),
      timeValid_(false),
      ntpConfigured_(false),
      tbHead_(0),
      tbCount_(0) {
  bootstrapBrokerHost_[0] = '\0';
  bootstrapUsername_[0] = '\0';
  bootstrapPassword_[0] = '\0';
  bootstrapClientId_[0] = '\0';
  bootstrapClaimTopic_[0] = '\0';
  bootstrapReplyPrefix_[0] = '\0';
  locationHint_[0] = '\0';
  resetFinalConfigInMemory();
}

void BbhIotFirmwareCore::dispatchMqttMessage(String& topic, String& payload) {
  if (activeInstance != nullptr) {
    activeInstance->onMqttMessage(topic, payload);
  }
}

void BbhIotFirmwareCore::setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("BBH IoT firmware core starting");

  activeInstance = this;
  initializePins();
  initializeSensorDefinitions();
  loadRuntimeConfig();
  loadAndIncrementBootSeq();
  initTelemetryBuffer();
  deviceMode_ = hasFinalConfig() ? DEVICE_MODE_RUNTIME : DEVICE_MODE_BOOTSTRAP;

  sensorAdapter_.begin();
  captureLatestReadings();

  if (!connectWifi()) {
    logEvent("WiFi not ready. MQTT will retry in loop.");
  }

  if (deviceMode_ == DEVICE_MODE_RUNTIME) {
    connectRuntimeMqtt();
  } else {
    connectBootstrapMqtt();
  }

  lastTelemetryAt_ = millis();
  lastStatusAt_ = millis();
}

void BbhIotFirmwareCore::loop() {
  handleFactoryResetButton();
  if (localServerStarted_) {
    localServer_.handleClient();
  }

  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
  }

  performPendingMqttReconnect();

  if (WiFi.status() == WL_CONNECTED) {
    ensureLocalServer();
    syncTimeIfNeeded();
    if (deviceMode_ == DEVICE_MODE_RUNTIME) {
      connectRuntimeMqtt();
    } else {
      connectBootstrapMqtt();
    }
  }

  mqtt_.loop();

  if (rebootRequested_) {
    delay(100);
    ESP.restart();
  }

  if (deviceMode_ == DEVICE_MODE_BOOTSTRAP) {
    if (mqtt_.connected() && (millis() - lastBootstrapClaimAt_ >= config_.timing.bootstrapClaimIntervalMs)) {
      publishBootstrapClaim();
    }
    return;
  }

  if (mqtt_.connected() && millis() - lastStatusAt_ >= config_.timing.statusIntervalMs) {
    publishRuntimeStatus("online");
    lastStatusAt_ = millis();
  }

  const unsigned long readingIntervalMs = static_cast<unsigned long>(readingIntervalMinutes_) * 60000UL;
  if (millis() - lastTelemetryAt_ >= readingIntervalMs) {
    String record;
    if (buildTelemetryRecord(record)) {
      // Send live only when online AND the backlog is already empty, so buffered
      // records always drain in chronological (FIFO) order.
      bool sentLive = mqtt_.connected() && tbCount_ == 0 && publishTelemetryRecord(record);
      if (sentLive) {
        lastPublishSummary_ = "telemetry: ok";
      } else if (telemetryBufferEnqueue(record)) {
        lastPublishSummary_ = String("telemetry: buffered (") + tbCount_ + " queued)";
      } else {
        lastPublishSummary_ = "telemetry: buffer unavailable, dropped";
      }
    }
    lastTelemetryAt_ = millis();
  }

  if (mqtt_.connected()) {
    drainTelemetryBuffer(5);
  }
}

void BbhIotFirmwareCore::initializePins() {
  pinMode(config_.pins.blueLedPin, OUTPUT);
  pinMode(config_.pins.redLedPin, OUTPUT);
  pinMode(config_.pins.resetButtonPin, INPUT_PULLUP);
  setBlueLed(false);
  setRedLed(false);
}

void BbhIotFirmwareCore::initializeSensorDefinitions() {
  BbhSensorDefinition definitions[kMaxSensors];
  size_t count = 0;
  sensorAdapter_.describeSensors(definitions, kMaxSensors, count);
  sensorDefCount_ = count > kMaxSensors ? kMaxSensors : count;

  for (size_t i = 0; i < sensorDefCount_; i++) {
    snprintf(sensorDefs_[i].sensorKey, sizeof(sensorDefs_[i].sensorKey), "%s", definitions[i].sensorKey);
    snprintf(sensorDefs_[i].sensorType, sizeof(sensorDefs_[i].sensorType), "%s", definitions[i].sensorType);
    snprintf(sensorDefs_[i].unit, sizeof(sensorDefs_[i].unit), "%s", definitions[i].unit);
  }
}

void BbhIotFirmwareCore::captureLatestReadings() {
  latestReadingCount_ = 0;
  sensorAdapter_.readTelemetry(latestReadings_, kMaxSensors, latestReadingCount_);
  if (latestReadingCount_ > kMaxSensors) {
    latestReadingCount_ = kMaxSensors;
  }
}

void BbhIotFirmwareCore::persistBootstrapConfig() {
  configStore_.begin("coldroom", false);
  configStore_.putString("bs_host", String(bootstrapBrokerHost_));
  configStore_.putUShort("bs_port", bootstrapBrokerPort_);
  configStore_.putString("bs_user", String(bootstrapUsername_));
  configStore_.putString("bs_pass", String(bootstrapPassword_));
  configStore_.putString("bs_client", String(bootstrapClientId_));
  configStore_.putString("bs_claim", String(bootstrapClaimTopic_));
  configStore_.putString("bs_reply", String(bootstrapReplyPrefix_));
  configStore_.putString("loc_hint", String(locationHint_));
  configStore_.putUShort("bs_interval", readingIntervalMinutes_);
  configStore_.putUChar("wifi_tx_pwr", wifiTxPower_);
  configStore_.end();
}

void BbhIotFirmwareCore::persistFinalConfig() {
  JsonDocument doc;
  JsonArray sensors = doc.to<JsonArray>();
  for (size_t i = 0; i < sensorDefCount_; i++) {
    JsonObject sensor = sensors.add<JsonObject>();
    sensor["sensor_key"] = sensorDefs_[i].sensorKey;
    sensor["sensor_type"] = sensorDefs_[i].sensorType;
    sensor["unit"] = sensorDefs_[i].unit;
  }
  String sensorsJson;
  serializeJson(doc, sensorsJson);

  configStore_.begin("coldroom", false);
  configStore_.putString("dev_key", String(deviceKey_));
  configStore_.putString("dev_name", String(deviceName_));
  configStore_.putString("dev_type", String(deviceType_));
  configStore_.putString("loc_code", String(locationCode_));
  configStore_.putString("rt_host", String(runtimeBrokerHost_));
  configStore_.putUShort("rt_port", runtimeBrokerPort_);
  configStore_.putString("rt_proto", String(runtimeProtocol_));
  configStore_.putString("rt_user", String(runtimeUsername_));
  configStore_.putString("rt_pass", String(runtimePassword_));
  configStore_.putString("rt_client", String(runtimeClientId_));
  configStore_.putString("rt_base", String(runtimeTopicBase_));
  configStore_.putString("rt_status", String(runtimeStatusTopic_));
  configStore_.putString("rt_tele", String(runtimeTelemetryTopic_));
  configStore_.putUShort("rt_qos", runtimeQos_);
  configStore_.putUShort("rt_keep", runtimeKeepaliveSeconds_);
  configStore_.putUShort("rt_exp", expectedIntervalMinutes_);
  configStore_.putUShort("rt_off", offlineAfterMinutes_);
  configStore_.putString("rt_sensors", sensorsJson);
  configStore_.putString("approved_at", approvedAt_);
  configStore_.end();
}

void BbhIotFirmwareCore::resetFinalConfigInMemory() {
  deviceKey_[0] = '\0';
  deviceName_[0] = '\0';
  deviceType_[0] = '\0';
  locationCode_[0] = '\0';
  runtimeBrokerHost_[0] = '\0';
  runtimeProtocol_[0] = '\0';
  runtimeUsername_[0] = '\0';
  runtimePassword_[0] = '\0';
  runtimeClientId_[0] = '\0';
  runtimeTopicBase_[0] = '\0';
  runtimeStatusTopic_[0] = '\0';
  runtimeTelemetryTopic_[0] = '\0';
  runtimeBrokerPort_ = 0;
  runtimeQos_ = 1;
  runtimeKeepaliveSeconds_ = 60;
  expectedIntervalMinutes_ = 0;
  offlineAfterMinutes_ = 0;
  approvedAt_ = "";
}

void BbhIotFirmwareCore::clearFinalConfig() {
  configStore_.begin("coldroom", false);
  configStore_.remove("dev_key");
  configStore_.remove("dev_name");
  configStore_.remove("dev_type");
  configStore_.remove("loc_code");
  configStore_.remove("rt_host");
  configStore_.remove("rt_port");
  configStore_.remove("rt_proto");
  configStore_.remove("rt_user");
  configStore_.remove("rt_pass");
  configStore_.remove("rt_client");
  configStore_.remove("rt_base");
  configStore_.remove("rt_status");
  configStore_.remove("rt_tele");
  configStore_.remove("rt_qos");
  configStore_.remove("rt_keep");
  configStore_.remove("rt_exp");
  configStore_.remove("rt_off");
  configStore_.remove("rt_sensors");
  configStore_.remove("approved_at");
  configStore_.end();

  resetFinalConfigInMemory();
  initializeSensorDefinitions();
}

void BbhIotFirmwareCore::loadRuntimeConfig() {
  snprintf(bootstrapBrokerHost_, sizeof(bootstrapBrokerHost_), "%s", config_.bootstrap.brokerHost);
  bootstrapBrokerPort_ = config_.bootstrap.brokerPort;
  snprintf(bootstrapUsername_, sizeof(bootstrapUsername_), "%s", config_.bootstrap.username);
  snprintf(bootstrapPassword_, sizeof(bootstrapPassword_), "%s", config_.bootstrap.password);
  snprintf(bootstrapClientId_, sizeof(bootstrapClientId_), "%s", config_.bootstrap.clientId);
  snprintf(bootstrapClaimTopic_, sizeof(bootstrapClaimTopic_), "%s", config_.bootstrap.claimTopic);
  snprintf(bootstrapReplyPrefix_, sizeof(bootstrapReplyPrefix_), "%s", config_.bootstrap.replyPrefix);
  snprintf(locationHint_, sizeof(locationHint_), "%s", config_.bootstrap.locationHint);
  resetFinalConfigInMemory();

  configStore_.begin("coldroom", true);
  String storedHost = configStore_.getString("bs_host", String(bootstrapBrokerHost_));
  uint16_t storedPort = configStore_.getUShort("bs_port", bootstrapBrokerPort_);
  String storedUser = configStore_.getString("bs_user", String(bootstrapUsername_));
  String storedPass = configStore_.getString("bs_pass", String(bootstrapPassword_));
  String storedClientId = configStore_.getString("bs_client", String(bootstrapClientId_));
  String storedClaim = configStore_.getString("bs_claim", String(bootstrapClaimTopic_));
  String storedReply = configStore_.getString("bs_reply", String(bootstrapReplyPrefix_));
  String storedHint = configStore_.getString("loc_hint", String(locationHint_));

  snprintf(bootstrapBrokerHost_, sizeof(bootstrapBrokerHost_), "%s", storedHost.c_str());
  bootstrapBrokerPort_ = storedPort > 0 ? storedPort : config_.bootstrap.brokerPort;
  snprintf(bootstrapUsername_, sizeof(bootstrapUsername_), "%s", storedUser.c_str());
  snprintf(bootstrapPassword_, sizeof(bootstrapPassword_), "%s", storedPass.c_str());
  snprintf(bootstrapClientId_, sizeof(bootstrapClientId_), "%s", storedClientId.c_str());
  snprintf(bootstrapClaimTopic_, sizeof(bootstrapClaimTopic_), "%s", storedClaim.c_str());
  snprintf(bootstrapReplyPrefix_, sizeof(bootstrapReplyPrefix_), "%s", storedReply.c_str());
  snprintf(locationHint_, sizeof(locationHint_), "%s", storedHint.c_str());

  readingIntervalMinutes_ = configStore_.getUShort("bs_interval", kDefaultReadingMinutes);
  if (readingIntervalMinutes_ < 1) {
    readingIntervalMinutes_ = kDefaultReadingMinutes;
  }
  if (readingIntervalMinutes_ > kMaxReadingMinutes) {
    readingIntervalMinutes_ = kMaxReadingMinutes;
  }

  wifiTxPower_ = configStore_.getUChar("wifi_tx_pwr", 78);
  if (wifiTxPower_ < 20 || wifiTxPower_ > 82) {
    wifiTxPower_ = 78;
  }

  snprintf(deviceKey_, sizeof(deviceKey_), "%s", configStore_.getString("dev_key", "").c_str());
  snprintf(deviceName_, sizeof(deviceName_), "%s", configStore_.getString("dev_name", "").c_str());
  snprintf(deviceType_, sizeof(deviceType_), "%s", configStore_.getString("dev_type", "").c_str());
  snprintf(locationCode_, sizeof(locationCode_), "%s", configStore_.getString("loc_code", "").c_str());
  snprintf(runtimeBrokerHost_, sizeof(runtimeBrokerHost_), "%s", configStore_.getString("rt_host", "").c_str());
  runtimeBrokerPort_ = configStore_.getUShort("rt_port", 0);
  snprintf(runtimeProtocol_, sizeof(runtimeProtocol_), "%s", configStore_.getString("rt_proto", "").c_str());
  snprintf(runtimeUsername_, sizeof(runtimeUsername_), "%s", configStore_.getString("rt_user", "").c_str());
  snprintf(runtimePassword_, sizeof(runtimePassword_), "%s", configStore_.getString("rt_pass", "").c_str());
  snprintf(runtimeClientId_, sizeof(runtimeClientId_), "%s", configStore_.getString("rt_client", "").c_str());
  snprintf(runtimeTopicBase_, sizeof(runtimeTopicBase_), "%s", configStore_.getString("rt_base", "").c_str());
  snprintf(runtimeStatusTopic_, sizeof(runtimeStatusTopic_), "%s", configStore_.getString("rt_status", "").c_str());
  snprintf(
      runtimeTelemetryTopic_,
      sizeof(runtimeTelemetryTopic_),
      "%s",
      configStore_.getString("rt_tele", "").c_str());
  runtimeQos_ = configStore_.getUShort("rt_qos", 1);
  runtimeKeepaliveSeconds_ = configStore_.getUShort("rt_keep", 60);
  expectedIntervalMinutes_ = configStore_.getUShort("rt_exp", 0);
  offlineAfterMinutes_ = configStore_.getUShort("rt_off", 0);
  approvedAt_ = configStore_.getString("approved_at", "");
  String sensorsJson = configStore_.getString("rt_sensors", "");
  configStore_.end();

  if (sensorsJson.length() > 0) {
    JsonDocument doc;
    if (deserializeJson(doc, sensorsJson) == DeserializationError::Ok && doc.is<JsonArray>()) {
      size_t count = 0;
      for (JsonObject sensor : doc.as<JsonArray>()) {
        if (count >= kMaxSensors) {
          break;
        }
        snprintf(sensorDefs_[count].sensorKey, sizeof(sensorDefs_[count].sensorKey), "%s", sensor["sensor_key"] | "");
        snprintf(sensorDefs_[count].sensorType, sizeof(sensorDefs_[count].sensorType), "%s", sensor["sensor_type"] | "");
        snprintf(sensorDefs_[count].unit, sizeof(sensorDefs_[count].unit), "%s", sensor["unit"] | "C");
        count++;
      }
      if (count > 0) {
        sensorDefCount_ = count;
      }
    }
  }

  if (!hasFinalConfig()) {
    resetFinalConfigInMemory();
  }
}

void BbhIotFirmwareCore::loadAndIncrementBootSeq() {
  // The backend dedups telemetry on (device_id, sensor_id, ingest_message_id).
  // A per-message counter that restarts at 0 on every boot would collide with
  // pre-reboot message_ids and be silently dropped, so we prefix each id with a
  // persistent, monotonically increasing boot sequence number.
  configStore_.begin("coldroom", false);
  bootSeq_ = configStore_.getUInt("boot_seq", 0) + 1;
  configStore_.putUInt("boot_seq", bootSeq_);
  configStore_.end();
  logEvent("Boot sequence: " + String(bootSeq_));
}

bool BbhIotFirmwareCore::hasFinalConfig() const {
  return !isBlank(deviceKey_) && !isBlank(runtimeClientId_) && !isBlank(runtimeStatusTopic_) &&
         !isBlank(runtimeTelemetryTopic_) && !isBlank(runtimeBrokerHost_) && !isBlank(runtimeUsername_) &&
         !isBlank(runtimePassword_) && runtimeBrokerPort_ > 0;
}

static const char* wifiStatusReason(wl_status_t status) {
  switch (status) {
    case WL_CONNECTED:       return "connected";
    case WL_NO_SSID_AVAIL:   return "network not found (wrong SSID, out of range, or 5GHz-only)";
    case WL_CONNECT_FAILED:  return "auth failed (wrong password?)";
    case WL_CONNECTION_LOST: return "connection lost (signal dropped)";
    case WL_DISCONNECTED:    return "disconnected / still negotiating";
    case WL_IDLE_STATUS:     return "idle";
    case WL_NO_SHIELD:       return "no wifi radio";
    default:                 return "unknown";
  }
}

bool BbhIotFirmwareCore::connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    ensureLocalServer();
    return true;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin();
  applyWifiPerformanceProfile();

  Serial.print("WiFi connecting");
  unsigned long startMs = millis();
  unsigned long lastBlinkMs = 0;
  bool redOn = false;

  setBlueLed(false);
  setRedLed(false);

  while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < config_.timing.wifiConnectTimeoutMs) {
    if (millis() - lastBlinkMs >= config_.timing.redBlinkMs) {
      redOn = !redOn;
      setRedLed(redOn);
      lastBlinkMs = millis();
    }
    delay(20);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    ensureLocalServer();
    logEvent("WiFi connected, IP: " + WiFi.localIP().toString() + ", RSSI: " + String(WiFi.RSSI()) + " dBm");
    return true;
  }

  setBlueLed(false);
  setRedLed(true);
  logEvent(String("WiFi connection failed: ") + wifiStatusReason(WiFi.status()) + ". Starting setup AP...");
  return runOnboardingPortal(false);
}

void BbhIotFirmwareCore::applyWifiPerformanceProfile() {
  WiFi.setTxPower((wifi_power_t)(wifiTxPower_ / 2));
#if BBH_WIFI_HIGH_PERFORMANCE
  // Keep the radio fully awake (no modem micro-naps) for stabler RSSI and faster
  // reconnects, and transmit at maximum power for better range. Higher idle
  // current, so this is opt-in for devices where power is not a concern.
  WiFi.setSleep(false);
  static bool logged = false;
  if (!logged) {
    logged = true;
    logEvent("WiFi high-performance profile enabled (modem sleep off).");
  }
#endif
}

bool BbhIotFirmwareCore::runOnboardingPortal(bool forcePortal) {
  WiFiManager wm;
  // Verbose portal + connection logging to Serial. The browser can't show live
  // STA-connect progress (the page is served from the AP while the radio tests
  // the new network), so the serial monitor is the real progress window.
  wm.setDebugOutput(true);
  wm.setConfigPortalTimeout(180);
  wm.setConnectTimeout(max(1UL, config_.timing.wifiConnectTimeoutMs / 1000UL));
  wm.setConnectRetries(1);

#if BBH_WIFI_HIGH_PERFORMANCE
  // The captive setup AP is hosted right next to whoever is onboarding, so it
  // never needs range. The high-power station profile is for reaching a distant
  // router; leaving it on for our own SoftAP only spikes peak current (brown-outs
  // that make the AP drop in and out) and heats the board on USB power. Drop TX
  // power once the portal AP comes up — applyWifiPerformanceProfile() below
  // restores the full station profile after we join the real network.
  wm.setAPCallback([](WiFiManager*) {
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
  });
#endif

  // Modern look & feel for the captive setup portal. Injected into <head> so it
  // overrides WiFiManager's built-in styling.
  static const char kPortalCss[] PROGMEM =
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<style>"
      ":root{--bbh:#0f62fe;}"
      "body{background:#eef2f7;font-family:'Segoe UI',Roboto,Arial,sans-serif;color:#1e2a38;}"
      ".wrap{max-width:440px;margin:0 auto;padding:10px 16px 28px;}"
      "h1{color:var(--bbh);text-align:center;font-weight:700;letter-spacing:.4px;margin:18px 0 4px;}"
      "h3{text-align:center;color:#5b6b7e;font-weight:500;margin:0 0 14px;}"
      "button,input,select{border-radius:10px;font-size:15px;}"
      "button{background:var(--bbh);border:0;color:#fff;font-weight:600;padding:12px;box-shadow:0 2px 6px rgba(15,98,254,.25);}"
      "button:hover{filter:brightness(1.07);}"
      "input,select{border:1px solid #cdd7e3;padding:11px;background:#fff;width:100%;}"
      "input:focus,select:focus{outline:none;border-color:var(--bbh);box-shadow:0 0 0 3px rgba(15,98,254,.15);}"
      "a{color:var(--bbh);}"
      ".msg{border-left:4px solid var(--bbh);background:#fff;border-radius:8px;padding:10px 12px;}"
      "</style>";
  wm.setCustomHeadElement(kPortalCss);

  // Simplified onboarding: only the essentials are user-facing. Broker port,
  // MQTT username, client id, claim topic and reply prefix keep their
  // compiled-in defaults (see BbhBootstrapDefaults) and can still be changed
  // later from the device's local web page if ever needed.
  char intervalBuffer[8];
  snprintf(intervalBuffer, sizeof(intervalBuffer), "%u", readingIntervalMinutes_);

  WiFiManagerParameter brokerHostParam("bs_host", "MQTT broker IP", bootstrapBrokerHost_, sizeof(bootstrapBrokerHost_) - 1);
  WiFiManagerParameter bootstrapPassParam("bs_pass", "MQTT password (blank = keep default)", "", sizeof(bootstrapPassword_) - 1);
  WiFiManagerParameter readingIntervalParam("bs_interval", "Reading interval (minutes)", intervalBuffer, sizeof(intervalBuffer) - 1);
  WiFiManagerParameter locationHintParam("loc_hint", "Location (optional)", locationHint_, sizeof(locationHint_) - 1);

  wm.addParameter(&brokerHostParam);
  wm.addParameter(&bootstrapPassParam);
  wm.addParameter(&readingIntervalParam);
  wm.addParameter(&locationHintParam);

  bool configured =
      forcePortal ? wm.startConfigPortal(buildSetupApName().c_str(), config_.ui.setupApPassword)
                  : wm.autoConnect(buildSetupApName().c_str(), config_.ui.setupApPassword);

  if (!configured) {
    setBlueLed(false);
    setRedLed(true);
    logEvent(String("Onboarding AP timed out or failed: ") + wifiStatusReason(WiFi.status()));
    return false;
  }

  applyWifiPerformanceProfile();

  copyTrimmed(bootstrapBrokerHost_, sizeof(bootstrapBrokerHost_), brokerHostParam.getValue());
  if (!isBlank(bootstrapPassParam.getValue())) {
    copyTrimmed(bootstrapPassword_, sizeof(bootstrapPassword_), bootstrapPassParam.getValue());
  }
  copyTrimmed(locationHint_, sizeof(locationHint_), locationHintParam.getValue());

  unsigned long mins = strtoul(readingIntervalParam.getValue(), nullptr, 10);
  if (mins >= 1 && mins <= kMaxReadingMinutes) {
    readingIntervalMinutes_ = static_cast<uint16_t>(mins);
  }

  if (isBlank(bootstrapBrokerHost_)) {
    copyTrimmed(bootstrapBrokerHost_, sizeof(bootstrapBrokerHost_), config_.bootstrap.brokerHost);
  }
  if (bootstrapBrokerPort_ == 0) {
    bootstrapBrokerPort_ = config_.bootstrap.brokerPort;
  }

  persistBootstrapConfig();
  requestMqttReconnect("bootstrap settings updated via onboarding portal");

  setBlueLed(true);
  setRedLed(false);
  logEvent("WiFi connected after onboarding, IP: " + WiFi.localIP().toString() + ", RSSI: " + String(WiFi.RSSI()) + " dBm");
  return true;
}

bool BbhIotFirmwareCore::connectBootstrapMqtt() {
  if (mqtt_.connected()) {
    return true;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  if (millis() - lastMqttAttemptAt_ < config_.timing.mqttRetryMs) {
    return false;
  }
  lastMqttAttemptAt_ = millis();

  mqtt_.begin(bootstrapBrokerHost_, bootstrapBrokerPort_, net_);
  mqtt_.setKeepAlive(60);
  mqtt_.onMessage(dispatchMqttMessage);

  logEvent("Connecting bootstrap MQTT " + String(bootstrapBrokerHost_) + ":" + String(bootstrapBrokerPort_));
  bool connected = false;
  if (isBlank(bootstrapUsername_) && isBlank(bootstrapPassword_)) {
    connected = mqtt_.connect(bootstrapClientId().c_str());
  } else {
    connected = mqtt_.connect(bootstrapClientId().c_str(), bootstrapUsername_, bootstrapPassword_);
  }

  if (!connected) {
    setBlueLed(false);
    setRedLed(true);
    logEvent("Bootstrap MQTT connect failed: " + mqttFailureSummary(mqtt_));
    return false;
  }

  String replyTopic = bootstrapReplyTopic();
  bool subscribed = mqtt_.subscribe(replyTopic, 1);
  logEvent(String("Bootstrap reply subscribe ") + (subscribed ? "ok " : "failed ") + replyTopic);
  bootstrapClaimPublished_ = false;
  setBlueLed(true);
  setRedLed(false);
  logEvent("Bootstrap MQTT connected.");

  // A retained approved config may arrive immediately after subscribing. Give it
  // a short window before creating a fresh claim that would rotate credentials.
  waitForBootstrapReply(1500UL);
  if (deviceMode_ != DEVICE_MODE_BOOTSTRAP) {
    return true;
  }

  publishBootstrapClaim();
  waitForBootstrapReply(30000UL);
  return true;
}

bool BbhIotFirmwareCore::connectRuntimeMqtt() {
  if (mqtt_.connected()) {
    return true;
  }
  if (WiFi.status() != WL_CONNECTED || !hasFinalConfig()) {
    return false;
  }
  if (millis() - lastMqttAttemptAt_ < config_.timing.mqttRetryMs) {
    return false;
  }
  lastMqttAttemptAt_ = millis();

  mqtt_.begin(runtimeBrokerHost_, runtimeBrokerPort_, net_);
  mqtt_.setKeepAlive(runtimeKeepaliveSeconds_);
  mqtt_.onMessage(dispatchMqttMessage);
  updateRuntimeOfflineWill();

  logEvent("Connecting runtime MQTT " + String(runtimeBrokerHost_) + ":" + String(runtimeBrokerPort_));
  bool connected = mqtt_.connect(runtimeClientId_, runtimeUsername_, runtimePassword_);
  if (!connected) {
    setBlueLed(false);
    setRedLed(true);
    logEvent("Runtime MQTT connect failed: " + mqttFailureSummary(mqtt_));
    if (isRuntimeAuthDenied(mqtt_)) {
      runtimeAuthFailures_++;
      if (runtimeAuthFailures_ < kMaxRuntimeAuthFailures) {
        // Treat the first few rejections as transient (e.g. broker mid-rotation
        // or a momentary ACL desync) and keep retrying with the stored creds,
        // rather than wiping the device back to bootstrap on a single failure.
        logEvent("Runtime MQTT auth denied (" + String(runtimeAuthFailures_) + "/" +
                 String(kMaxRuntimeAuthFailures) + "). Retrying with stored credentials.");
      } else {
        logEvent("Runtime MQTT auth denied repeatedly. Clearing stored runtime config and returning to bootstrap.");
        runtimeAuthFailures_ = 0;
        clearFinalConfig();
        deviceMode_ = DEVICE_MODE_BOOTSTRAP;
        lastMqttAttemptAt_ = 0;
        requestMqttReconnect("runtime auth denied repeatedly; switching back to bootstrap MQTT");
      }
    }
    return false;
  }

  runtimeAuthFailures_ = 0;

  // Subscribe to the per-device config topic so the backend can push new
  // credentials to a live device (graceful rotation) without forcing a full
  // re-onboard. The device applies them, ACKs on its state topic, then reconnects.
  String runtimeConfigTopic = String(runtimeTopicBase_) + "/config";
  if (mqtt_.subscribe(runtimeConfigTopic, runtimeQos_)) {
    logEvent("Runtime config subscribe ok " + runtimeConfigTopic);
  } else {
    logEvent("Runtime config subscribe FAILED " + runtimeConfigTopic);
  }

  publishRuntimeStatus("online");
  setBlueLed(true);
  setRedLed(false);
  logEvent("Runtime MQTT connected.");
  return true;
}

void BbhIotFirmwareCore::performPendingMqttReconnect() {
  if (!mqttReconnectRequested_) {
    return;
  }
  mqttReconnectRequested_ = false;
  if (mqtt_.connected()) {
    mqtt_.disconnect();
  }
  lastMqttAttemptAt_ = 0;
  lastStatusAt_ = 0;
}

void BbhIotFirmwareCore::publishBootstrapClaim() {
  if (!mqtt_.connected() || deviceMode_ != DEVICE_MODE_BOOTSTRAP) {
    return;
  }

  JsonDocument doc;
  doc["hardware_id"] = hardwareId();
  doc["firmware_version"] = config_.firmwareVersion;
  doc["location_hint"] = locationHint_;
  doc["ip_address"] = WiFi.localIP().toString();

  JsonArray sensors = doc["sensors"].to<JsonArray>();
  for (size_t i = 0; i < sensorDefCount_; i++) {
    JsonObject sensor = sensors.add<JsonObject>();
    sensor["sensor_key"] = sensorDefs_[i].sensorKey;
    sensor["sensor_type"] = sensorDefs_[i].sensorType;
    sensor["unit"] = sensorDefs_[i].unit;
  }

  String payload;
  serializeJson(doc, payload);
  bool ok = mqtt_.publish(String(bootstrapClaimTopic_), payload, false, 1);
  lastBootstrapClaimAt_ = millis();
  bootstrapClaimPublished_ = ok;
  lastBootstrapStatus_ = ok ? "claim_published" : "claim_publish_failed";
  lastPublishSummary_ = String("bootstrap claim: ") + (ok ? "ok" : "failed");
  logEvent(String("Bootstrap claim publish ") + (ok ? "ok" : "failed"));
}

bool BbhIotFirmwareCore::applyApprovedConfig(JsonDocument& doc) {
  JsonObject device = doc["device"];
  JsonObject mqttCfg = doc["mqtt"];
  JsonObject broker = mqttCfg["broker"];
  JsonObject topics = doc["topics"];

  const char* approvedDeviceKey = device["key"] | "";
  const char* approvedUser = mqttCfg["username"] | "";
  const char* approvedPass = mqttCfg["password"] | "";
  const char* approvedClientId = mqttCfg["client_id"] | "";
  const char* approvedTopicBase = mqttCfg["topic_base"] | "";
  const char* approvedStatusTopic = topics["status"] | "";
  const char* approvedTelemetryTopic = topics["telemetry"] | "";
  const char* approvedBrokerHost = broker["host"] | bootstrapBrokerHost_;
  uint16_t approvedBrokerPort = broker["port"] | bootstrapBrokerPort_;

  if (strlen(approvedDeviceKey) == 0 || strlen(approvedUser) == 0 || strlen(approvedPass) == 0 ||
      strlen(approvedClientId) == 0 || strlen(approvedTopicBase) == 0 || strlen(approvedStatusTopic) == 0 ||
      strlen(approvedTelemetryTopic) == 0 || strlen(approvedBrokerHost) == 0 || approvedBrokerPort == 0) {
    logEvent("Approved config missing required MQTT fields.");
    return false;
  }

  snprintf(deviceKey_, sizeof(deviceKey_), "%s", approvedDeviceKey);
  snprintf(deviceName_, sizeof(deviceName_), "%s", device["name"] | "");
  snprintf(deviceType_, sizeof(deviceType_), "%s", device["type"] | "");
  snprintf(locationCode_, sizeof(locationCode_), "%s", device["location_code"] | "");
  snprintf(runtimeUsername_, sizeof(runtimeUsername_), "%s", approvedUser);
  snprintf(runtimePassword_, sizeof(runtimePassword_), "%s", approvedPass);
  snprintf(runtimeClientId_, sizeof(runtimeClientId_), "%s", approvedClientId);
  snprintf(runtimeTopicBase_, sizeof(runtimeTopicBase_), "%s", approvedTopicBase);
  snprintf(runtimeStatusTopic_, sizeof(runtimeStatusTopic_), "%s", approvedStatusTopic);
  snprintf(runtimeTelemetryTopic_, sizeof(runtimeTelemetryTopic_), "%s", approvedTelemetryTopic);
  snprintf(runtimeBrokerHost_, sizeof(runtimeBrokerHost_), "%s", approvedBrokerHost);
  snprintf(runtimeProtocol_, sizeof(runtimeProtocol_), "%s", broker["protocol"] | "mqtt");
  runtimeBrokerPort_ = approvedBrokerPort;
  runtimeQos_ = mqttCfg["qos"] | 1;
  runtimeKeepaliveSeconds_ = mqttCfg["keepalive_seconds"] | 60;
  expectedIntervalMinutes_ = device["expected_interval_minutes"] | 0;
  offlineAfterMinutes_ = device["offline_after_minutes"] | 0;
  approvedAt_ = String((const char*)(doc["approved_at"] | ""));

  JsonArray approvedSensors = doc["sensors"];
  if (!approvedSensors.isNull()) {
    size_t count = 0;
    for (JsonObject sensor : approvedSensors) {
      if (count >= kMaxSensors) {
        break;
      }
      const char* key = sensor["sensor_key"] | "";
      if (strlen(key) == 0) {
        continue;
      }
      snprintf(sensorDefs_[count].sensorKey, sizeof(sensorDefs_[count].sensorKey), "%s", key);
      snprintf(sensorDefs_[count].sensorType, sizeof(sensorDefs_[count].sensorType), "%s", sensor["sensor_type"] | "");
      snprintf(sensorDefs_[count].unit, sizeof(sensorDefs_[count].unit), "%s", sensor["unit"] | "C");
      count++;
    }
    if (count > 0) {
      sensorDefCount_ = count;
    }
  }

  persistFinalConfig();
  return hasFinalConfig();
}

void BbhIotFirmwareCore::publishRuntimeStatus(const char* state) {
  if (!mqtt_.connected() || !hasFinalConfig()) {
    return;
  }

  JsonDocument doc;
  doc["state"] = state;
  doc["firmware_version"] = config_.firmwareVersion;
  doc["ip_address"] = WiFi.localIP().toString();
  doc["device_key"] = deviceKey_;
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["uptime_seconds"] = millis() / 1000UL;

  String payload;
  serializeJson(doc, payload);
  bool ok = mqtt_.publish(String(runtimeStatusTopic_), payload, false, runtimeQos_);
  lastPublishSummary_ = String("status: ") + (ok ? "ok" : "failed");
}

void BbhIotFirmwareCore::updateRuntimeOfflineWill() {
  JsonDocument doc;
  doc["state"] = "offline";
  doc["firmware_version"] = config_.firmwareVersion;
  doc["device_key"] = deviceKey_;

  String payload;
  serializeJson(doc, payload);
  mqtt_.setWill(runtimeStatusTopic_, payload.c_str(), false, runtimeQos_);
}

bool BbhIotFirmwareCore::buildTelemetryRecord(String& outJson) {
  if (!hasFinalConfig()) {
    return false;
  }

  captureLatestReadings();
  if (latestReadingCount_ == 0) {
    logEvent("Skipping telemetry: no valid sensor readings available.");
    return false;
  }

  JsonDocument doc;
  char messageId[96];
  snprintf(
      messageId,
      sizeof(messageId),
      "%s-%lu-%06lu",
      deviceKey_,
      static_cast<unsigned long>(bootSeq_),
      static_cast<unsigned long>(++runtimeMessageCounter_));
  doc["message_id"] = messageId;
  doc["firmware_version"] = config_.firmwareVersion;
  // Stamp the reading time now, while the sample is fresh, so a record that gets
  // buffered during an outage keeps its true timestamp instead of the flush time.
  if (isTimeValid()) {
    doc["reading_time"] = currentIso8601();
  }

  JsonArray readings = doc["readings"].to<JsonArray>();
  size_t publishedCount = 0;
  for (size_t i = 0; i < latestReadingCount_; i++) {
    if (!latestReadings_[i].valid) {
      continue;
    }
    JsonObject reading = readings.add<JsonObject>();
    reading["sensor_key"] = latestReadings_[i].sensorKey;
    reading["value"] = latestReadings_[i].value;
    reading["unit"] = latestReadings_[i].unit;
    publishedCount++;
  }

  if (publishedCount == 0) {
    logEvent("Skipping telemetry: all sensor readings invalid.");
    return false;
  }

  outJson = "";
  serializeJson(doc, outJson);
  return true;
}

bool BbhIotFirmwareCore::publishTelemetryRecord(const String& json) {
  if (!mqtt_.connected() || !hasFinalConfig()) {
    return false;
  }
  return mqtt_.publish(String(runtimeTelemetryTopic_), json, false, runtimeQos_);
}

void BbhIotFirmwareCore::drainTelemetryBuffer(size_t maxRecords) {
  if (!mqtt_.connected() || !hasFinalConfig()) {
    return;
  }

  size_t sent = 0;
  while (tbCount_ > 0 && sent < maxRecords) {
    String json;
    if (!telemetryBufferPeek(json)) {
      // Unreadable slot: drop it so the queue can make progress.
      logEvent("Dropping unreadable buffered telemetry record.");
      telemetryBufferPopFront();
      continue;
    }
    if (!publishTelemetryRecord(json)) {
      break;  // Stay buffered and retry on a later loop.
    }
    telemetryBufferPopFront();
    sent++;
  }

  if (sent > 0) {
    lastPublishSummary_ = String("telemetry: flushed ") + sent + ", " + tbCount_ + " queued";
    logEvent(String("Flushed ") + sent + " buffered record(s); " + tbCount_ + " remaining.");
  }
}

void BbhIotFirmwareCore::initTelemetryBuffer() {
  tbHead_ = 0;
  tbCount_ = 0;
  fsReady_ = LittleFS.begin(true);  // format on first use / mount failure
  if (!fsReady_) {
    logEvent("LittleFS mount failed; telemetry buffering disabled.");
    return;
  }

  // Pre-size the ring file so every slot seek/write stays within the file.
  File f = LittleFS.open(kTelemetryBufferPath, LittleFS.exists(kTelemetryBufferPath) ? "r+" : "w+");
  if (f) {
    uint32_t needed = static_cast<uint32_t>(kTelemetryBufferCapacity) * kTelemetrySlotSize;
    if (f.size() < needed) {
      f.seek(needed - 1);
      uint8_t zero = 0;
      f.write(&zero, 1);
    }
    f.close();
  } else {
    fsReady_ = false;
    logEvent("Telemetry buffer file open failed; buffering disabled.");
    return;
  }

  configStore_.begin("coldroom", true);
  tbHead_ = configStore_.getUShort("tb_head", 0);
  tbCount_ = configStore_.getUShort("tb_count", 0);
  configStore_.end();
  if (tbHead_ >= kTelemetryBufferCapacity) {
    tbHead_ = 0;
  }
  if (tbCount_ > kTelemetryBufferCapacity) {
    tbCount_ = kTelemetryBufferCapacity;
  }
  logEvent(String("Telemetry buffer ready (") + tbCount_ + " queued).");
}

void BbhIotFirmwareCore::persistTelemetryBufferMeta() {
  configStore_.begin("coldroom", false);
  configStore_.putUShort("tb_head", tbHead_);
  configStore_.putUShort("tb_count", tbCount_);
  configStore_.end();
}

bool BbhIotFirmwareCore::telemetryBufferEnqueue(const String& json) {
  if (!fsReady_) {
    return false;
  }
  if (json.length() + 2 > kTelemetrySlotSize) {
    logEvent("Telemetry record exceeds slot size; dropped.");
    return false;
  }

  if (tbCount_ >= kTelemetryBufferCapacity) {
    // Bounded ring: discard the oldest record to make room for the newest.
    tbHead_ = (tbHead_ + 1) % kTelemetryBufferCapacity;
    tbCount_--;
    logEvent("Telemetry buffer full; dropped oldest record.");
  }

  uint16_t tail = (tbHead_ + tbCount_) % kTelemetryBufferCapacity;
  File f = LittleFS.open(kTelemetryBufferPath, "r+");
  if (!f) {
    logEvent("Telemetry buffer open failed (enqueue).");
    return false;
  }
  if (!f.seek(static_cast<uint32_t>(tail) * kTelemetrySlotSize)) {
    f.close();
    return false;
  }
  uint16_t len = static_cast<uint16_t>(json.length());
  uint8_t lenBytes[2] = {static_cast<uint8_t>(len & 0xFF), static_cast<uint8_t>((len >> 8) & 0xFF)};
  f.write(lenBytes, 2);
  f.write(reinterpret_cast<const uint8_t*>(json.c_str()), len);
  f.close();

  tbCount_++;
  persistTelemetryBufferMeta();
  return true;
}

bool BbhIotFirmwareCore::telemetryBufferPeek(String& outJson) {
  if (!fsReady_ || tbCount_ == 0) {
    return false;
  }

  File f = LittleFS.open(kTelemetryBufferPath, "r");
  if (!f) {
    return false;
  }
  if (!f.seek(static_cast<uint32_t>(tbHead_) * kTelemetrySlotSize)) {
    f.close();
    return false;
  }
  uint8_t lenBytes[2];
  if (f.read(lenBytes, 2) != 2) {
    f.close();
    return false;
  }
  uint16_t len = static_cast<uint16_t>(lenBytes[0] | (lenBytes[1] << 8));
  if (len == 0 || static_cast<size_t>(len) + 2 > kTelemetrySlotSize) {
    f.close();
    return false;
  }
  char buf[kTelemetrySlotSize];
  int got = f.read(reinterpret_cast<uint8_t*>(buf), len);
  f.close();
  if (got != static_cast<int>(len)) {
    return false;
  }
  buf[len] = '\0';
  outJson = String(buf);
  return true;
}

void BbhIotFirmwareCore::telemetryBufferPopFront() {
  if (tbCount_ == 0) {
    return;
  }
  tbHead_ = (tbHead_ + 1) % kTelemetryBufferCapacity;
  tbCount_--;
  persistTelemetryBufferMeta();
}

void BbhIotFirmwareCore::syncTimeIfNeeded() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (!ntpConfigured_) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");  // UTC; reading_time is UTC
    ntpConfigured_ = true;
    logEvent("NTP time sync started (UTC).");
  }
  if (!timeValid_ && time(nullptr) > kMinValidEpoch) {
    timeValid_ = true;
    logEvent("NTP time acquired.");
  }
}

bool BbhIotFirmwareCore::isTimeValid() {
  if (timeValid_) {
    return true;
  }
  if (time(nullptr) > kMinValidEpoch) {
    timeValid_ = true;
  }
  return timeValid_;
}

String BbhIotFirmwareCore::currentIso8601() {
  time_t now = time(nullptr);
  struct tm tmInfo;
  gmtime_r(&now, &tmInfo);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmInfo);
  return String(buf);
}

void BbhIotFirmwareCore::onMqttMessage(String& topic, String& payload) {
  if (deviceMode_ == DEVICE_MODE_RUNTIME) {
    if (topic == String(runtimeTopicBase_) + "/config") {
      handleRuntimeConfigMessage(payload);
    }
    return;
  }

  if (deviceMode_ != DEVICE_MODE_BOOTSTRAP || topic != bootstrapReplyTopic()) {
    return;
  }

  logEvent("Bootstrap MQTT RX " + topic);
  JsonDocument doc;
  if (deserializeJson(doc, payload) != DeserializationError::Ok) {
    lastBootstrapStatus_ = "invalid_reply";
    logEvent("Bootstrap reply parse failed.");
    return;
  }

  const char* replyHardwareId = doc["hardware_id"] | "";
  if (strlen(replyHardwareId) > 0 && String(replyHardwareId) != hardwareId()) {
    logEvent("Bootstrap reply ignored: hardware_id mismatch.");
    return;
  }

  lastClaimId_ = String((const char*)(doc["claim_id"] | ""));
  String status = String((const char*)(doc["status"] | "unknown"));
  status.toLowerCase();
  lastBootstrapStatus_ = status;

  if (status == "pending") {
    bootstrapClaimPending_ = true;
    logEvent("Bootstrap claim pending.");
    return;
  }

  if (status != "approved") {
    logEvent("Bootstrap reply status: " + status);
    return;
  }

  String kind = String((const char*)(doc["kind"] | ""));
  if (kind != "device_config") {
    logEvent("Approved bootstrap reply ignored: kind is not device_config.");
    return;
  }

  if (!applyApprovedConfig(doc)) {
    lastBootstrapStatus_ = "approved_invalid";
    logEvent("Approved bootstrap config failed validation.");
    return;
  }

  bootstrapClaimPending_ = false;
  lastBootstrapStatus_ = "approved";
  logEvent("Bootstrap approved. Final MQTT config stored.");
  deviceMode_ = DEVICE_MODE_RUNTIME;
  requestMqttReconnect("switching from bootstrap MQTT to runtime MQTT");
}

void BbhIotFirmwareCore::handleRuntimeConfigMessage(const String& payload) {
  logEvent("Runtime config RX (credential push).");
  JsonDocument doc;
  if (deserializeJson(doc, payload) != DeserializationError::Ok) {
    logEvent("Runtime config parse failed.");
    return;
  }

  const char* msgHardwareId = doc["hardware_id"] | "";
  if (strlen(msgHardwareId) > 0 && String(msgHardwareId) != hardwareId()) {
    logEvent("Runtime config ignored: hardware_id mismatch.");
    return;
  }

  String kind = String((const char*)(doc["kind"] | ""));
  if (kind != "device_config") {
    logEvent("Runtime config ignored: kind is not device_config.");
    return;
  }

  // Correlate the ACK with the backend's rotation request, if one was supplied.
  String rotationId = String((const char*)(doc["rotation_id"] | ""));

  if (!applyApprovedConfig(doc)) {
    logEvent("Runtime config failed validation; keeping previous credentials.");
    return;
  }

  // ACK on the live (still-authenticated) session BEFORE reconnecting, so the
  // backend swaps the broker password only after the device has persisted the
  // new credentials. The reconnect then comes up on the new credentials; the
  // auth-retry tolerance covers the brief window while the broker catches up.
  runtimeAuthFailures_ = 0;
  publishConfigAck(rotationId);
  logEvent("Runtime credentials updated via config push.");
  requestMqttReconnect("runtime credentials updated via config push");
}

void BbhIotFirmwareCore::publishConfigAck(const String& rotationId) {
  if (!mqtt_.connected() || isBlank(runtimeTopicBase_)) {
    return;
  }

  JsonDocument doc;
  doc["event"] = "config_applied";
  doc["device_key"] = deviceKey_;
  if (rotationId.length() > 0) {
    doc["rotation_id"] = rotationId;
  }
  doc["firmware_version"] = config_.firmwareVersion;

  String payload;
  serializeJson(doc, payload);
  String stateTopic = String(runtimeTopicBase_) + "/state";
  bool ok = mqtt_.publish(stateTopic, payload, false, runtimeQos_);
  mqtt_.loop();  // nudge the client to flush the ACK before the pending reconnect.
  logEvent(String("Config ACK ") + (ok ? "sent" : "failed") + " on " + stateTopic);
}

void BbhIotFirmwareCore::ensureLocalServer() {
  if (localServerStarted_ || WiFi.status() != WL_CONNECTED) {
    return;
  }

  localServer_.on("/", HTTP_GET, [this]() { handleLocalStatusPage(); });
  localServer_.on("/api/status", HTTP_GET, [this]() { handleLocalStatusJson(); });
  localServer_.on("/setup", HTTP_POST, [this]() { handleLocalSetupSave(); });
  localServer_.on("/rebootstrap", HTTP_POST, [this]() { handleLocalRebootstrap(); });
  localServer_.onNotFound([this]() {
    if (!requireLocalAuth()) {
      return;
    }
    localServer_.send(404, "text/plain", "Not found");
  });
  localServer_.begin();
  localServerStarted_ = true;
  logEvent("Local status server started on port 80");
}

bool BbhIotFirmwareCore::requireLocalAuth() {
  if (localServer_.authenticate(config_.ui.localUsername, config_.ui.localPassword)) {
    return true;
  }
  localServer_.requestAuthentication(BASIC_AUTH, "BBH IoT Device", "Login required");
  return false;
}

void BbhIotFirmwareCore::clearProvisioningAndRestart() {
  logEvent("Factory reset: clearing Wi-Fi and stored config.");
  clearFinalConfig();
  configStore_.begin("coldroom", false);
  configStore_.clear();
  configStore_.end();

  WiFiManager wm;
  wm.resetSettings();

  setBlueLed(false);
  setRedLed(false);
  for (int i = 0; i < 3; i++) {
    setRedLed(true);
    delay(250);
    setRedLed(false);
    delay(250);
  }

  delay(300);
  ESP.restart();
}

void BbhIotFirmwareCore::handleFactoryResetButton() {
  bool pressed = (digitalRead(config_.pins.resetButtonPin) == LOW);

  if (!pressed) {
    if (resetPressActive_) {
      resetPressActive_ = false;
      resetNotified_ = false;
      setRedLed(false);
    }
    return;
  }

  if (!resetPressActive_) {
    resetPressActive_ = true;
    resetPressStartMs_ = millis();
    resetLastBlinkMs_ = millis();
    resetLedOn_ = false;
    resetNotified_ = false;
  }

  unsigned long heldMs = millis() - resetPressStartMs_;
  if (!resetNotified_ && heldMs >= 300) {
    logEvent("Reset button held. Keep holding for factory reset...");
    resetNotified_ = true;
  }

  if (millis() - resetLastBlinkMs_ >= config_.timing.redBlinkMs) {
    resetLedOn_ = !resetLedOn_;
    setRedLed(resetLedOn_);
    setBlueLed(false);
    resetLastBlinkMs_ = millis();
  }

  if (heldMs >= config_.timing.resetHoldMs) {
    clearProvisioningAndRestart();
  }
}

void BbhIotFirmwareCore::handleLocalStatusPage() {
  if (!requireLocalAuth()) {
    return;
  }

  bool saved = localServer_.hasArg("saved");
  bool rebootstrap = localServer_.hasArg("rebootstrap");
  bool runtimeMode = deviceMode_ == DEVICE_MODE_RUNTIME;
  captureLatestReadings();

  String html;
  html.reserve(13000);
  html += "<!doctype html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>BBH IoT Device</title>";
  html += "<style>*{box-sizing:border-box;}body{font-family:'Segoe UI',Roboto,Arial,sans-serif;margin:0;background:#eef2f7;color:#1e2a38;}";
  html += ".wrap{max-width:1040px;margin:0 auto;padding:18px 16px 28px;}.grid{display:grid;gap:14px;grid-template-columns:1fr;}";
  html += ".panel{background:#fff;border:1px solid #e3e9f1;border-radius:12px;padding:16px;box-shadow:0 1px 3px rgba(16,40,80,.06),0 4px 12px rgba(16,40,80,.04);}table{border-collapse:collapse;width:100%;font-size:13px;}";
  html += "td,th{padding:8px 4px;border-bottom:1px solid #eef2f6;text-align:left;vertical-align:top;}td:first-child{width:40%;color:#506173;font-weight:600;}";
  html += "h1,h2{margin:0 0 12px 0;}h1{font-size:22px;color:#0f62fe;letter-spacing:.3px;}h2{font-size:16px;}p{margin:0 0 10px 0;color:#506173;font-size:13px;}";
  html += "label{display:block;font-size:12px;font-weight:600;color:#506173;margin:10px 0 4px;}input{width:100%;padding:11px;border:1px solid #cfd8e3;border-radius:10px;font-size:14px;}input:focus{outline:none;border-color:#0f62fe;box-shadow:0 0 0 3px rgba(15,98,254,.15);}";
  html += "input[type='range']{padding:0;height:8px;}.slider-label{display:flex;justify-content:space-between;}.slider-val{font-weight:700;color:#0f62fe;}";
  html += ".actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px;}.btn{display:inline-block;padding:10px 16px;border-radius:10px;border:0;background:#0f62fe;color:#fff;font-weight:600;cursor:pointer;text-decoration:none;box-shadow:0 2px 6px rgba(15,98,254,.25);}";
  html += ".btn.secondary{background:#e9eef5;color:#1e2a38;}.btn.warn{background:#9f1239;color:#fff;}.notice{padding:10px 12px;border-radius:6px;margin-bottom:10px;font-size:13px;}";
  html += ".notice.ok{background:#e8f7ec;color:#185c37;border:1px solid #b8e0c2;}.notice.info{background:#eef4ff;color:#24417a;border:1px solid #c6d6fb;}";
  html += ".pill{display:inline-block;padding:3px 8px;border-radius:999px;font-size:12px;font-weight:700;}.pill.ok{background:#dff7e8;color:#166534;}.pill.bad{background:#fee2e2;color:#991b1b;}.pill.wait{background:#fef3c7;color:#92400e;}";
  html += "details{background:#fff;border:1px solid #d8e0ea;border-radius:8px;padding:10px;}summary{font-weight:700;cursor:pointer;}pre{white-space:pre-wrap;background:#101820;color:#dbe2f0;padding:10px;border-radius:6px;max-height:300px;overflow:auto;font-size:12px;line-height:1.35;margin:0;}";
  html += "@media (min-width:820px){.grid{grid-template-columns:1fr 1fr;} .wide{grid-column:1 / -1;}}</style>";
  html += "<script>function updateSliderVal(id,val){document.getElementById(id+'_val').textContent=val+' dBm';}</script>";
  html += "</head><body><div class='wrap'><div class='grid'>";

  html += "<div class='panel wide'>";
  html += "<h1>BBH IoT Device</h1>";
  html += "<p>" + htmlEscape(String(locationHint_)) + "</p>";
  if (saved) {
    html += "<div class='notice ok'>Bootstrap settings saved. ";
    html += runtimeMode ? "These settings are stored for future bootstrap use." : "The device will retry bootstrap MQTT with the new values.";
    html += "</div>";
  }
  if (rebootstrap) {
    html += "<div class='notice ok'>Stored runtime credentials cleared. The device will request a fresh approved config through bootstrap.</div>";
  } else if (runtimeMode) {
    html += "<div class='notice info'>Registered device. Bootstrap settings are kept for recovery and reprovisioning.</div>";
  }
  html += "<div class='actions'><a class='btn secondary' href='/'>Refresh page</a><a class='btn secondary' href='/api/status'>View JSON status</a></div>";
  html += "</div>";

  html += "<div class='panel'><h2>Status</h2><table>";
  html += "<tr><td>Firmware</td><td>" + String(config_.firmwareVersion) + "</td></tr>";
  html += "<tr><td>Hardware ID</td><td>" + htmlEscape(hardwareId()) + "</td></tr>";
  html += "<tr><td>Mode</td><td><span class='pill " + String(runtimeMode ? "ok" : "wait") + "'>" + String(runtimeMode ? "runtime" : "bootstrap") + "</span></td></tr>";
  html += "<tr><td>Device Key</td><td>" + htmlEscape(isBlank(deviceKey_) ? String("-") : String(deviceKey_)) + "</td></tr>";
  html += "<tr><td>WiFi</td><td><span class='pill " + String(WiFi.status() == WL_CONNECTED ? "ok" : "bad") + "'>" + String(WiFi.status() == WL_CONNECTED ? "connected" : "disconnected") + "</span></td></tr>";
  html += "<tr><td>MQTT</td><td><span class='pill " + String(mqtt_.connected() ? "ok" : "bad") + "'>" + String(mqtt_.connected() ? "connected" : "disconnected") + "</span></td></tr>";
  html += "<tr><td>IP Address</td><td>" + WiFi.localIP().toString() + "</td></tr>";
  html += "<tr><td>Last Publish</td><td>" + htmlEscape(lastPublishSummary_) + "</td></tr>";
  html += "<tr><td>Buffered Telemetry</td><td>" + String(tbCount_) + "</td></tr>";
  html += "<tr><td>Time Synced</td><td>" + String(isTimeValid() ? "yes (UTC)" : "no") + "</td></tr>";
  html += "<tr><td>Uptime</td><td>" + formatUptimeDhM(millis()) + "</td></tr>";
  html += "</table></div>";

  html += "<div class='panel'><h2>Runtime MQTT</h2><table>";
  html += "<tr><td>Runtime Status Topic</td><td>" + htmlEscape(String(runtimeStatusTopic_)) + "</td></tr>";
  html += "<tr><td>Runtime Telemetry Topic</td><td>" + htmlEscape(String(runtimeTelemetryTopic_)) + "</td></tr>";
  html += "<tr><td>Bootstrap Status</td><td>" + htmlEscape(lastBootstrapStatus_) + "</td></tr>";
  html += "<tr><td>Claim ID</td><td>" + htmlEscape(lastClaimId_) + "</td></tr>";
  html += "</table><form method='post' action='/rebootstrap' class='actions'><button class='btn secondary' type='submit'>Refresh Runtime Credentials</button></form></div>";

  html += "<div class='panel wide'><h2>Latest Readings</h2><table><tr><th>Sensor</th><th>Value</th><th>Unit</th><th>Status</th></tr>";
  if (latestReadingCount_ == 0) {
    html += "<tr><td colspan='4'>No readings captured yet.</td></tr>";
  } else {
    for (size_t i = 0; i < latestReadingCount_; i++) {
      html += "<tr><td>" + htmlEscape(String(latestReadings_[i].sensorKey)) + "</td><td>";
      html += latestReadings_[i].valid ? String(latestReadings_[i].value, 2) : String("-");
      html += "</td><td>" + htmlEscape(String(latestReadings_[i].unit)) + "</td><td>";
      html += latestReadings_[i].valid ? "<span class='pill ok'>valid</span>" : "<span class='pill bad'>invalid</span>";
      html += "</td></tr>";
    }
  }
  html += "</table></div>";

  html += "<details class='wide' " + String(runtimeMode ? "" : "open") + "><summary>MQTT Setup</summary>";
  html += "<p>Point the device at the MQTT broker. Only the broker IP is required; the password is optional and never shown after saving.</p>";
  html += "<form method='post' action='/setup'>";
  html += "<label for='bs_host'>MQTT broker IP</label>";
  html += "<input id='bs_host' name='bs_host' value='" + htmlEscape(String(bootstrapBrokerHost_)) + "' maxlength='79' required>";
  html += "<label for='bs_pass'>MQTT password</label>";
  html += "<input id='bs_pass' name='bs_pass' type='password' value='' maxlength='79' placeholder='Leave blank to keep saved password'>";
  html += "<label for='bs_interval'>Reading interval (minutes)</label>";
  html += "<input id='bs_interval' name='bs_interval' type='number' min='1' max='1440' value='" + String(readingIntervalMinutes_) + "' required>";
  html += "<div class='slider-label'><label for='wifi_tx_pwr'>WiFi transmit power</label><span class='slider-val' id='wifi_tx_pwr_val'>" + String(wifiTxPower_) + " dBm</span></div>";
  html += "<input id='wifi_tx_pwr' name='wifi_tx_pwr' type='range' min='20' max='82' value='" + String(wifiTxPower_) + "' onchange=\"updateSliderVal('wifi_tx_pwr')\" oninput=\"updateSliderVal('wifi_tx_pwr')\">";
  html += "<label for='loc_hint'>Location (optional)</label>";
  html += "<input id='loc_hint' name='loc_hint' value='" + htmlEscape(String(locationHint_)) + "' maxlength='63'>";
  html += "<div class='actions'><button class='btn' type='submit'>Save Bootstrap Settings</button></div>";
  html += "</form></details>";

  html += "<div class='panel wide'><h2>Event Log</h2><pre>";
  if (eventLogCount_ == 0) {
    html += "(no events yet)";
  } else {
    size_t start = (eventLogHead_ + kLogCapacity - eventLogCount_) % kLogCapacity;
    for (size_t i = 0; i < eventLogCount_; i++) {
      size_t idx = (start + i) % kLogCapacity;
      html += htmlEscape(eventLog_[idx]);
      html += "\n";
    }
  }
  html += "</pre></div></div></div></body></html>";

  localServer_.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  localServer_.sendHeader("Pragma", "no-cache");
  localServer_.send(200, "text/html", html);
}

void BbhIotFirmwareCore::handleLocalStatusJson() {
  if (!requireLocalAuth()) {
    return;
  }

  JsonDocument doc;
  doc["firmware_version"] = config_.firmwareVersion;
  doc["hardware_id"] = hardwareId();
  doc["mode"] = deviceMode_ == DEVICE_MODE_RUNTIME ? "runtime" : "bootstrap";
  doc["device_key"] = deviceKey_;
  doc["location_hint"] = locationHint_;
  doc["bootstrap_client_id"] = bootstrapClientId();
  doc["bootstrap_claim_topic"] = bootstrapClaimTopic_;
  doc["bootstrap_reply_topic"] = bootstrapReplyTopic();
  doc["runtime_status_topic"] = runtimeStatusTopic_;
  doc["runtime_telemetry_topic"] = runtimeTelemetryTopic_;
  doc["wifi_connected"] = (WiFi.status() == WL_CONNECTED);
  doc["mqtt_connected"] = mqtt_.connected();
  doc["ip_address"] = WiFi.localIP().toString();
  doc["last_bootstrap_status"] = lastBootstrapStatus_;
  doc["claim_id"] = lastClaimId_;
  doc["uptime_ms"] = millis();
  doc["buffered_telemetry"] = tbCount_;
  doc["time_valid"] = isTimeValid();

  JsonArray sensors = doc["sensors"].to<JsonArray>();
  for (size_t i = 0; i < sensorDefCount_; i++) {
    JsonObject sensor = sensors.add<JsonObject>();
    sensor["sensor_key"] = sensorDefs_[i].sensorKey;
    sensor["sensor_type"] = sensorDefs_[i].sensorType;
    sensor["unit"] = sensorDefs_[i].unit;
  }

  JsonArray readings = doc["latest_readings"].to<JsonArray>();
  for (size_t i = 0; i < latestReadingCount_; i++) {
    JsonObject reading = readings.add<JsonObject>();
    reading["sensor_key"] = latestReadings_[i].sensorKey;
    reading["value"] = latestReadings_[i].value;
    reading["unit"] = latestReadings_[i].unit;
    reading["valid"] = latestReadings_[i].valid;
  }

  JsonArray logs = doc["logs"].to<JsonArray>();
  size_t start = (eventLogHead_ + kLogCapacity - eventLogCount_) % kLogCapacity;
  for (size_t i = 0; i < eventLogCount_; i++) {
    size_t idx = (start + i) % kLogCapacity;
    logs.add(eventLog_[idx]);
  }

  String out;
  serializeJson(doc, out);
  localServer_.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  localServer_.sendHeader("Pragma", "no-cache");
  localServer_.send(200, "application/json", out);
}

void BbhIotFirmwareCore::handleLocalSetupSave() {
  if (!requireLocalAuth()) {
    return;
  }

  copyTrimmed(bootstrapBrokerHost_, sizeof(bootstrapBrokerHost_), localServer_.arg("bs_host").c_str());
  String submittedBootstrapPassword = localServer_.arg("bs_pass");
  submittedBootstrapPassword.trim();
  if (submittedBootstrapPassword.length() > 0) {
    copyTrimmed(bootstrapPassword_, sizeof(bootstrapPassword_), submittedBootstrapPassword.c_str());
  }
  copyTrimmed(locationHint_, sizeof(locationHint_), localServer_.arg("loc_hint").c_str());

  String intervalArg = localServer_.arg("bs_interval");
  intervalArg.trim();
  if (intervalArg.length() > 0) {
    unsigned long mins = strtoul(intervalArg.c_str(), nullptr, 10);
    if (mins >= 1 && mins <= kMaxReadingMinutes) {
      readingIntervalMinutes_ = static_cast<uint16_t>(mins);
    }
  }

  String wifiTxPwrArg = localServer_.arg("wifi_tx_pwr");
  wifiTxPwrArg.trim();
  if (wifiTxPwrArg.length() > 0) {
    unsigned long pwr = strtoul(wifiTxPwrArg.c_str(), nullptr, 10);
    if (pwr >= 20 && pwr <= 82) {
      wifiTxPower_ = static_cast<uint8_t>(pwr);
    }
  }

  // Broker port, MQTT username, client id, claim topic and reply prefix are no
  // longer user-facing; they keep their saved values, falling back to the
  // compiled-in defaults if anything is somehow blank.
  if (isBlank(bootstrapBrokerHost_)) {
    copyTrimmed(bootstrapBrokerHost_, sizeof(bootstrapBrokerHost_), config_.bootstrap.brokerHost);
  }
  if (bootstrapBrokerPort_ == 0) {
    bootstrapBrokerPort_ = config_.bootstrap.brokerPort;
  }
  if (isBlank(bootstrapUsername_)) {
    copyTrimmed(bootstrapUsername_, sizeof(bootstrapUsername_), config_.bootstrap.username);
  }
  if (isBlank(bootstrapClientId_)) {
    copyTrimmed(bootstrapClientId_, sizeof(bootstrapClientId_), config_.bootstrap.clientId);
  }
  if (isBlank(bootstrapClaimTopic_)) {
    copyTrimmed(bootstrapClaimTopic_, sizeof(bootstrapClaimTopic_), config_.bootstrap.claimTopic);
  }
  if (isBlank(bootstrapReplyPrefix_)) {
    copyTrimmed(bootstrapReplyPrefix_, sizeof(bootstrapReplyPrefix_), config_.bootstrap.replyPrefix);
  }

  persistBootstrapConfig();
  logEvent("Bootstrap settings updated from local portal.");
  if (deviceMode_ == DEVICE_MODE_BOOTSTRAP) {
    requestMqttReconnect("bootstrap settings updated from local portal");
  }

  localServer_.sendHeader("Location", "/?saved=1", true);
  localServer_.sendHeader("Cache-Control", "no-store");
  localServer_.send(303, "text/plain", "");
}

void BbhIotFirmwareCore::handleLocalRebootstrap() {
  if (!requireLocalAuth()) {
    return;
  }

  logEvent("Manual runtime credential refresh requested from local portal.");
  clearFinalConfig();
  deviceMode_ = DEVICE_MODE_BOOTSTRAP;
  bootstrapClaimPublished_ = false;
  bootstrapClaimPending_ = false;
  lastBootstrapClaimAt_ = 0;
  lastMqttAttemptAt_ = 0;
  requestMqttReconnect("manual runtime credential refresh");

  localServer_.sendHeader("Location", "/?rebootstrap=1", true);
  localServer_.sendHeader("Cache-Control", "no-store");
  localServer_.send(303, "text/plain", "");
}

void BbhIotFirmwareCore::requestMqttReconnect(const String& reason) {
  mqttReconnectRequested_ = true;
  logEvent("MQTT reconnect scheduled: " + reason);
}

void BbhIotFirmwareCore::logEvent(const String& message) {
  String line = String(millis()) + "ms " + message;
  Serial.println(line);
  eventLog_[eventLogHead_] = line;
  eventLogHead_ = (eventLogHead_ + 1) % kLogCapacity;
  if (eventLogCount_ < kLogCapacity) {
    eventLogCount_++;
  }
}

void BbhIotFirmwareCore::setBlueLed(bool on) {
  digitalWrite(config_.pins.blueLedPin, on ? HIGH : LOW);
}

void BbhIotFirmwareCore::setRedLed(bool on) {
  digitalWrite(config_.pins.redLedPin, on ? HIGH : LOW);
}

String BbhIotFirmwareCore::hardwareId() const {
  uint64_t mac = ESP.getEfuseMac();
  uint8_t bytes[6] = {
      static_cast<uint8_t>(mac >> 40),
      static_cast<uint8_t>(mac >> 32),
      static_cast<uint8_t>(mac >> 24),
      static_cast<uint8_t>(mac >> 16),
      static_cast<uint8_t>(mac >> 8),
      static_cast<uint8_t>(mac)};
  char out[18];
  snprintf(out, sizeof(out), "%02X:%02X:%02X:%02X:%02X:%02X", bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
  return String(out);
}

String BbhIotFirmwareCore::hardwareIdCompact() const {
  String compact = hardwareId();
  compact.replace(":", "");
  return compact;
}

String BbhIotFirmwareCore::bootstrapReplyTopic() const {
  return String(bootstrapReplyPrefix_) + "/" + sanitizeMqttToken(hardwareId());
}

String BbhIotFirmwareCore::bootstrapClientId() const {
  if (!isBlank(bootstrapClientId_)) {
    return String(bootstrapClientId_);
  }
  return String("bootstrap-") + hardwareIdCompact();
}

String BbhIotFirmwareCore::sanitizeMqttToken(const String& value) const {
  String candidate = value;
  candidate.trim();

  String out;
  out.reserve(candidate.length());
  bool lastDash = false;

  for (size_t i = 0; i < candidate.length(); i++) {
    char c = candidate.charAt(i);
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-';

    if (ok) {
      out += c;
      lastDash = false;
    } else if (!lastDash) {
      out += '-';
      lastDash = true;
    }
  }

  while (out.startsWith("-")) {
    out.remove(0, 1);
  }
  while (out.endsWith("-")) {
    out.remove(out.length() - 1);
  }

  return out.length() > 0 ? out : "unknown-device";
}

void BbhIotFirmwareCore::waitForBootstrapReply(unsigned long timeoutMs) {
  if (!mqtt_.connected() || deviceMode_ != DEVICE_MODE_BOOTSTRAP) {
    return;
  }

  unsigned long startedAt = millis();
  while (mqtt_.connected() && deviceMode_ == DEVICE_MODE_BOOTSTRAP && millis() - startedAt < timeoutMs) {
    mqtt_.loop();
    if (lastBootstrapStatus_ == "pending" || lastBootstrapStatus_ == "approved" || lastBootstrapStatus_ == "approved_invalid") {
      return;
    }
    if (localServerStarted_) {
      localServer_.handleClient();
    }
    delay(10);
  }
}

String BbhIotFirmwareCore::formatUptimeDhM(unsigned long ms) const {
  unsigned long totalMinutes = ms / 60000UL;
  unsigned long days = totalMinutes / 1440UL;
  unsigned long hours = (totalMinutes % 1440UL) / 60UL;
  unsigned long minutes = totalMinutes % 60UL;
  return String(days) + "d " + String(hours) + "h " + String(minutes) + "m";
}

String BbhIotFirmwareCore::htmlEscape(const String& input) const {
  String out;
  out.reserve(input.length() + 16);
  for (size_t i = 0; i < input.length(); i++) {
    char c = input.charAt(i);
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

String BbhIotFirmwareCore::buildSetupApName() const {
  return String(config_.setupApPrefix) + "-" + hardwareIdCompact();
}

void BbhIotFirmwareCore::copyTrimmed(char* target, size_t targetSize, const char* source) {
  String value = source == nullptr ? String("") : String(source);
  value.trim();
  snprintf(target, targetSize, "%s", value.c_str());
}

bool BbhIotFirmwareCore::isBlank(const char* value) {
  if (value == nullptr) {
    return true;
  }
  String candidate(value);
  candidate.trim();
  return candidate.length() == 0;
}
