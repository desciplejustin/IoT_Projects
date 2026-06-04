#include <OneWire.h>
#include <DallasTemperature.h>
#include <vector>

#if !SENSOR_ID_SCAN_ONLY
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <WebServer.h>
#endif

const char* DEFAULT_API_URL = "http://10.24.16.142:5010/api/iot/v1/readings";
const char* DEFAULT_DEVICE_KEY = "";
const char* DEFAULT_DEVICE_TOKEN = "";
const char* DEFAULT_LOCATION_HINT = "";
const char* FIRMWARE_VERSION = "1.0.0";
const char* LOCAL_UI_USER = "TinMan";
const char* LOCAL_UI_PASS = "TinMan@2026";

const size_t API_URL_MAX = 160;
const size_t DEVICE_KEY_MAX = 64;
const size_t DEVICE_TOKEN_MAX = 160;
const size_t LOCATION_HINT_MAX = 64;

char apiUrl[API_URL_MAX];
char deviceKey[DEVICE_KEY_MAX];
char deviceToken[DEVICE_TOKEN_MAX];
char locationHint[LOCATION_HINT_MAX];

const int ONE_WIRE_PIN = 4;
unsigned long sendMs = 30000;
unsigned long lastSend = 0;
uint32_t messageCounter = 0;
const int BLUE_LED_PIN = 2;
const int RED_LED_PIN = 15;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;
const unsigned long RED_BLINK_MS = 250;
const int RESET_BUTTON_PIN = 0;
const unsigned long RESET_HOLD_MS = 5000;
const unsigned long CLAIM_RETRY_MS = 30000;

#if !SENSOR_ID_SCAN_ONLY
Preferences configStore;
String activeClaimId = "";
unsigned long nextClaimPollAt = 0;
unsigned long nextClaimCreateAt = 0;
bool resetPressActive = false;
unsigned long resetPressStartMs = 0;
unsigned long resetLastBlinkMs = 0;
bool resetLedOn = false;
bool resetNotified = false;
WebServer localServer(80);
bool localServerStarted = false;
float lastProbe1C = NAN;
float lastProbe2C = NAN;
int lastPostCode = -999;
String lastPostResp = "";
unsigned long lastPostAtMs = 0;
const size_t LOG_CAPACITY = 50;
String eventLog[LOG_CAPACITY];
size_t eventLogHead = 0;
size_t eventLogCount = 0;
#endif

const DeviceAddress PROBE_1_ADDR = {0x28, 0x7C, 0x34, 0x79, 0xA2, 0x16, 0x03, 0x46};
const DeviceAddress PROBE_2_ADDR = {0x28, 0xC3, 0x36, 0x79, 0xA2, 0x00, 0x03, 0xA0};

OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature ds18b20(&oneWire);

String addressToString(const DeviceAddress address) {
  char out[24];
  snprintf(
      out,
      sizeof(out),
      "%02X%02X%02X%02X%02X%02X%02X%02X",
      address[0],
      address[1],
      address[2],
      address[3],
      address[4],
      address[5],
      address[6],
      address[7]);
  return String(out);
}

void printSensorInventory() {
  uint8_t sensorCount = ds18b20.getDeviceCount();
  Serial.print("DS18B20 detected: ");
  Serial.println(sensorCount);

  for (uint8_t i = 0; i < sensorCount; i++) {
    DeviceAddress addr;
    if (!ds18b20.getAddress(addr, i)) {
      Serial.print("Sensor ");
      Serial.print(i);
      Serial.println(" address read failed");
      continue;
    }

    bool familyOk = (addr[0] == 0x28);
    bool crcOk = (OneWire::crc8(addr, 7) == addr[7]);

    Serial.print("Sensor ");
    Serial.print(i);
    Serial.print(" ROM: ");
    Serial.println(addressToString(addr));
    Serial.print("  family_ok: ");
    Serial.println(familyOk ? "true" : "false");
    Serial.print("  crc_ok: ");
    Serial.println(crcOk ? "true" : "false");
  }
}

float readTempCByIndex(uint8_t index) {
  float tempC = ds18b20.getTempCByIndex(index);
  return tempC;
}

float readTempCByAddress(const DeviceAddress address) {
  float tempC = ds18b20.getTempC(address);
  return tempC;
}

#if SENSOR_ID_SCAN_ONLY

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("ESP32 DS18B20 scan mode");
  Serial.print("OneWire pin: GPIO");
  Serial.println(ONE_WIRE_PIN);

  ds18b20.begin();
  printSensorInventory();

  if (ds18b20.getDeviceCount() < 2) {
    Serial.println("Warning: expected 2 sensors on the bus.");
  }
}

void loop() {
  ds18b20.requestTemperatures();

  uint8_t sensorCount = ds18b20.getDeviceCount();
  for (uint8_t i = 0; i < sensorCount; i++) {
    float t = readTempCByIndex(i);
    Serial.print("Sensor ");
    Serial.print(i);
    Serial.print(" temp C: ");
    Serial.println(t);
  }

  Serial.println("---");
  delay(3000);
}

#else

void setBlueLed(bool on) {
  digitalWrite(BLUE_LED_PIN, on ? HIGH : LOW);
}

void setRedLed(bool on) {
  digitalWrite(RED_LED_PIN, on ? HIGH : LOW);
}

bool connectWifi();
String hardwareId();
bool hasDeviceCredentials();

void logEvent(const String& message) {
  String line = String(millis()) + "ms " + message;
  Serial.println(line);
  eventLog[eventLogHead] = line;
  eventLogHead = (eventLogHead + 1) % LOG_CAPACITY;
  if (eventLogCount < LOG_CAPACITY) {
    eventLogCount++;
  }
}

bool requireLocalAuth() {
  if (localServer.authenticate(LOCAL_UI_USER, LOCAL_UI_PASS)) {
    return true;
  }
  localServer.requestAuthentication(BASIC_AUTH, "ColdRoom Device", "Login required");
  return false;
}

String htmlEscape(const String& input) {
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

String formatUptimeDhM(unsigned long ms) {
  unsigned long totalMinutes = ms / 60000UL;
  unsigned long days = totalMinutes / 1440UL;
  unsigned long hours = (totalMinutes % 1440UL) / 60UL;
  unsigned long minutes = totalMinutes % 60UL;

  return String(days) + "d " + String(hours) + "h " + String(minutes) + "m";
}

void handleLocalStatusPage() {
  if (!requireLocalAuth()) {
    return;
  }

  String html;
  html.reserve(5200);
  html += "<!doctype html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='5'>";
  html += "<title>ColdRoom Device</title>";
  html += "<style>";
  html += "*{box-sizing:border-box;}";
  html += "body{font-family:Segoe UI,Arial,sans-serif;margin:0;background:#f4f7fb;color:#1e2a38;}";
  html += ".wrap{max-width:940px;margin:0 auto;padding:14px;}";
  html += "h1{font-size:20px;margin:0 0 10px;line-height:1.25;}";
  html += ".sub{font-size:12px;color:#5b6b7f;margin-bottom:10px;}";
  html += ".grid{display:grid;gap:10px;grid-template-columns:1fr;}";
  html += ".card{background:#fff;border:1px solid #d8e0ea;border-radius:10px;padding:12px;}";
  html += "h2{font-size:14px;margin:0 0 8px;}";
  html += "table{border-collapse:collapse;width:100%;font-size:13px;}";
  html += "td{padding:6px 4px;border-bottom:1px solid #eef2f6;vertical-align:top;}";
  html += "td:first-child{width:44%;color:#506173;font-weight:600;}";
  html += "td:last-child{word-break:break-word;}";
  html += "pre{white-space:pre-wrap;background:#0d1117;color:#dbe2f0;padding:10px;border-radius:8px;max-height:300px;overflow:auto;font-size:12px;line-height:1.35;margin:0;}";
  html += "a{color:#0a63d8;text-decoration:none;font-size:13px;}";
  html += "a:hover{text-decoration:underline;}";
  html += "@media (min-width:800px){.grid{grid-template-columns:1fr 1fr;} .card.logs{grid-column:1 / -1;}}";
  html += "</style></head><body>";
  html += "<div class='wrap'>";
  html += "<h1>ColdRoom Device Local Status</h1>";
  html += "<div class='sub'>Auto-refresh every 5 seconds</div>";
  html += "<div class='grid'>";
  html += "<div class='card'><h2>Live Status</h2><table>";
  html += "<tr><td>Firmware</td><td>" + String(FIRMWARE_VERSION) + "</td></tr>";
  html += "<tr><td>Hardware ID</td><td>" + hardwareId() + "</td></tr>";
  html += "<tr><td>IP Address</td><td>" + WiFi.localIP().toString() + "</td></tr>";
  html += "<tr><td>WiFi</td><td>" + String(WiFi.status() == WL_CONNECTED ? "connected" : "disconnected") + "</td></tr>";
  html += "<tr><td>Claim ID</td><td>" + htmlEscape(activeClaimId.length() ? activeClaimId : String("none")) + "</td></tr>";
  html += "<tr><td>Has Credentials</td><td>" + String(hasDeviceCredentials() ? "yes" : "no") + "</td></tr>";
  html += "<tr><td>Probe 1 (C)</td><td>" + String(lastProbe1C, 2) + "</td></tr>";
  html += "<tr><td>Probe 2 (C)</td><td>" + String(lastProbe2C, 2) + "</td></tr>";
  html += "<tr><td>Last POST code</td><td>" + String(lastPostCode) + "</td></tr>";
  html += "<tr><td>Last POST at (ms)</td><td>" + String(lastPostAtMs) + "</td></tr>";
  html += "</table><div style='margin-top:8px;'><a href='/api/status'>View JSON status</a></div></div>";

  html += "<div class='card'><h2>Quick Stats</h2><table>";
  html += "<tr><td>Uptime</td><td>" + formatUptimeDhM(millis()) + "</td></tr>";
  html += "<tr><td>Last POST code</td><td>" + String(lastPostCode) + "</td></tr>";
  html += "<tr><td>Recent POST response</td><td>" + htmlEscape(lastPostResp) + "</td></tr>";
  html += "</table></div>";

  html += "<div class='card logs'><h2>Recent Events</h2><pre>";
  if (eventLogCount == 0) {
    html += "(no events yet)";
  } else {
    size_t start = (eventLogHead + LOG_CAPACITY - eventLogCount) % LOG_CAPACITY;
    for (size_t i = 0; i < eventLogCount; i++) {
      size_t idx = (start + i) % LOG_CAPACITY;
      html += htmlEscape(eventLog[idx]);
      html += "\n";
    }
  }
  html += "</pre></div></div></div></body></html>";

  localServer.send(200, "text/html", html);
}

void handleLocalStatusJson() {
  if (!requireLocalAuth()) {
    return;
  }

  JsonDocument doc;
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["hardware_id"] = hardwareId();
  doc["ip_address"] = WiFi.localIP().toString();
  doc["wifi_connected"] = (WiFi.status() == WL_CONNECTED);
  doc["has_credentials"] = hasDeviceCredentials();
  doc["active_claim_id"] = activeClaimId;
  doc["probe_1_c"] = lastProbe1C;
  doc["probe_2_c"] = lastProbe2C;
  doc["last_post_code"] = lastPostCode;
  doc["last_post_at_ms"] = lastPostAtMs;
  doc["uptime_ms"] = millis();

  JsonArray logs = doc["logs"].to<JsonArray>();
  size_t start = (eventLogHead + LOG_CAPACITY - eventLogCount) % LOG_CAPACITY;
  for (size_t i = 0; i < eventLogCount; i++) {
    size_t idx = (start + i) % LOG_CAPACITY;
    logs.add(eventLog[idx]);
  }

  String out;
  serializeJson(doc, out);
  localServer.send(200, "application/json", out);
}

void ensureLocalServer() {
  if (localServerStarted || WiFi.status() != WL_CONNECTED) {
    return;
  }
  localServer.on("/", HTTP_GET, handleLocalStatusPage);
  localServer.on("/api/status", HTTP_GET, handleLocalStatusJson);
  localServer.onNotFound([]() {
    if (!requireLocalAuth()) {
      return;
    }
    localServer.send(404, "text/plain", "Not found");
  });
  localServer.begin();
  localServerStarted = true;
  logEvent("Local status server started on port 80");
}

bool hasDeviceCredentials() {
  return strlen(deviceKey) > 0 && strlen(deviceToken) > 0;
}

void persistRuntimeConfig() {
  configStore.begin("coldroom", false);
  configStore.putString("api_url", String(apiUrl));
  configStore.putString("device_key", String(deviceKey));
  configStore.putString("device_token", String(deviceToken));
  configStore.putString("location_hint", String(locationHint));
  configStore.end();
}

String hardwareId() {
  uint64_t chip = ESP.getEfuseMac();
  char out[32];
  snprintf(out, sizeof(out), "esp32-%04X%08X", (uint16_t)(chip >> 32), (uint32_t)chip);
  return String(out);
}

String claimCreateUrl() {
  String base = String(apiUrl);
  int idx = base.lastIndexOf("/readings");
  if (idx > 0) {
    base = base.substring(0, idx);
  }
  return base + "/device-claims";
}

void clearProvisioningAndRestart() {
  logEvent("Factory reset: clearing Wi-Fi and device credentials.");
  configStore.begin("coldroom", false);
  configStore.clear();
  configStore.end();

  WiFiManager wm;
  wm.resetSettings();

  delay(300);
  ESP.restart();
}

void handleFactoryResetButton() {
  bool pressed = (digitalRead(RESET_BUTTON_PIN) == LOW);

  if (!pressed) {
    if (resetPressActive) {
      resetPressActive = false;
      resetNotified = false;
      setRedLed(false);
    }
    return;
  }

  if (!resetPressActive) {
    resetPressActive = true;
    resetPressStartMs = millis();
    resetLastBlinkMs = millis();
    resetLedOn = false;
    resetNotified = false;
  }

  unsigned long heldMs = millis() - resetPressStartMs;
  if (!resetNotified && heldMs >= 300) {
    logEvent("Reset button held. Keep holding for factory reset...");
    resetNotified = true;
  }

  if (millis() - resetLastBlinkMs >= RED_BLINK_MS) {
    resetLedOn = !resetLedOn;
    setRedLed(resetLedOn);
    setBlueLed(false);
    resetLastBlinkMs = millis();
  }

  if (heldMs >= RESET_HOLD_MS) {
    clearProvisioningAndRestart();
  }
}

bool beginDeviceClaim() {
  if (strlen(apiUrl) == 0) {
    logEvent("Cannot claim: API URL is empty.");
    return false;
  }

  HTTPClient http;
  String url = claimCreateUrl();
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");

  JsonDocument doc;
  doc["hardware_id"] = hardwareId();
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["location_hint"] = locationHint;
  doc["ip_address"] = WiFi.localIP().toString();

  JsonArray sensors = doc["sensors"].to<JsonArray>();
  JsonObject s1 = sensors.add<JsonObject>();
  s1["sensor_key"] = "probe_1";
  s1["sensor_type"] = "temperature";
  s1["unit"] = "C";

  JsonObject s2 = sensors.add<JsonObject>();
  s2["sensor_key"] = "probe_2";
  s2["sensor_type"] = "temperature";
  s2["unit"] = "C";

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  String resp = http.getString();
  logEvent("Claim create code: " + String(code));
  logEvent("Claim create response: " + resp);

  if (code < 200 || code >= 300) {
    http.end();
    return false;
  }

  JsonDocument out;
  DeserializationError err = deserializeJson(out, resp);
  if (err) {
    logEvent("Claim create parse failed.");
    http.end();
    return false;
  }

  JsonVariant data = out.as<JsonVariant>();
  if (!out["data"].isNull()) {
    data = out["data"];
  }

  if (data["claim_id"].is<const char*>()) {
    activeClaimId = String((const char*)data["claim_id"]);
  } else {
    activeClaimId = String((long)data["claim_id"].as<long>());
  }

  unsigned long pollSec = data["poll_after_seconds"] | 5;
  nextClaimPollAt = millis() + (pollSec * 1000UL);
  logEvent("Claim pending. Code: " + String((const char*)(data["claim_code"] | "")));
  logEvent("Claim expires at: " + String((const char*)(data["expires_at"] | "")));

  http.end();
  return true;
}

bool pollClaimStatus() {
  if (activeClaimId.length() == 0 || millis() < nextClaimPollAt) {
    return false;
  }

  HTTPClient http;
  String url = claimCreateUrl() + "/" + activeClaimId;
  http.begin(url);
  http.addHeader("Accept", "application/json");

  int code = http.GET();
  String resp = http.getString();
  logEvent("Claim poll code: " + String(code));
  logEvent("Claim poll response: " + resp);

  if (code < 200 || code >= 300) {
    if (code == 404 && resp.indexOf("claim_not_found") >= 0) {
      logEvent("Claim not found on server. Creating a new claim.");
      activeClaimId = "";
      nextClaimCreateAt = millis();
      http.end();
      return false;
    }
    nextClaimPollAt = millis() + 5000;
    http.end();
    return false;
  }

  JsonDocument out;
  DeserializationError err = deserializeJson(out, resp);
  if (err) {
    nextClaimPollAt = millis() + 5000;
    http.end();
    return false;
  }

  JsonVariant data = out.as<JsonVariant>();
  if (!out["data"].isNull()) {
    data = out["data"];
  }

  String status = String((const char*)(data["status"] | "pending"));
  status.toLowerCase();

  if (status == "pending") {
    unsigned long pollSec = data["poll_after_seconds"] | 5;
    nextClaimPollAt = millis() + (pollSec * 1000UL);
    http.end();
    return false;
  }

  if (status == "approved") {
    snprintf(deviceKey, DEVICE_KEY_MAX, "%s", (const char*)(data["device_key"] | ""));
    snprintf(deviceToken, DEVICE_TOKEN_MAX, "%s", (const char*)(data["device_token"] | ""));
    int expectedIntervalMinutes = data["expected_interval_minutes"] | 0;
    if (expectedIntervalMinutes > 0) {
      sendMs = (unsigned long)expectedIntervalMinutes * 60000UL;
    }
    persistRuntimeConfig();
    activeClaimId = "";
    logEvent("Claim approved. Device credentials stored.");
    http.end();
    return hasDeviceCredentials();
  }

  if (status == "expired" || status == "rejected") {
    logEvent("Claim ended with status: " + status);
    activeClaimId = "";
    nextClaimCreateAt = millis() + CLAIM_RETRY_MS;
  }

  http.end();
  return false;
}

bool ensureDeviceCredentials() {
  if (hasDeviceCredentials()) {
    return true;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (!connectWifi()) {
      return false;
    }
  }

  if (activeClaimId.length() == 0 && millis() >= nextClaimCreateAt) {
    if (!beginDeviceClaim()) {
      nextClaimCreateAt = millis() + CLAIM_RETRY_MS;
      return false;
    }
  }

  return pollClaimStatus();
}

bool runOnboardingPortal(bool forcePortal) {
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);

  WiFiManagerParameter apiUrlParam("api_url", "Backend API URL", apiUrl, API_URL_MAX - 1);
  wm.addParameter(&apiUrlParam);

  const char* updateApMenuHtml = "<form action='/wifi' method='get'><button>Update AP</button></form><br/>\n";
  std::vector<const char*> menu = {"wifi", "info", "custom", "exit", "sep", "update"};
  wm.setCustomMenuHTML(updateApMenuHtml);
  wm.setMenu(menu);

  bool configured = forcePortal
      ? wm.startConfigPortal("COLDROOM-SETUP", "coldroom123")
      : wm.autoConnect("COLDROOM-SETUP", "coldroom123");

  if (!configured) {
    setBlueLed(false);
    setRedLed(true);
    logEvent("Onboarding AP timed out or failed.");
    return false;
  }

  const char* newApi = apiUrlParam.getValue();
  if (newApi != nullptr && strlen(newApi) > 0 && strncmp(apiUrl, newApi, API_URL_MAX) != 0) {
    snprintf(apiUrl, API_URL_MAX, "%s", newApi);
    persistRuntimeConfig();
    logEvent("Backend API URL updated via setup AP: " + String(apiUrl));
  }

  setBlueLed(true);
  setRedLed(false);
  logEvent("WiFi connected after onboarding, IP: " + WiFi.localIP().toString());
  return true;
}

bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    setBlueLed(true);
    setRedLed(false);
    ensureLocalServer();
    return true;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin();

  Serial.print("WiFi connecting");
  unsigned long startMs = millis();
  unsigned long lastBlinkMs = 0;
  bool redOn = false;

  setBlueLed(false);
  setRedLed(false);

  while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < WIFI_CONNECT_TIMEOUT_MS) {
    if (millis() - lastBlinkMs >= RED_BLINK_MS) {
      redOn = !redOn;
      setRedLed(redOn);
      lastBlinkMs = millis();
    }
    delay(20);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    setBlueLed(true);
    setRedLed(false);
    logEvent("WiFi connected, IP: " + WiFi.localIP().toString());
    ensureLocalServer();
    return true;
  }

  setBlueLed(false);
  setRedLed(true);
  logEvent("WiFi connection failed (timeout). Starting setup AP...");

  return runOnboardingPortal(false);
}

bool postReading(float tempProbe1, float tempProbe2) {
  if (WiFi.status() != WL_CONNECTED) {
    if (!connectWifi()) {
      return false;
    }
  }

  if (strlen(apiUrl) == 0 || strlen(deviceKey) == 0 || strlen(deviceToken) == 0) {
    if (!ensureDeviceCredentials()) {
      logEvent("Device credentials pending approval.");
      return false;
    }
  }

  HTTPClient http;
  http.begin(apiUrl);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("x-device-token", deviceToken);

  JsonDocument doc;
  doc["device_key"] = deviceKey;
  doc["message_id"] = String("esp32-coldroom-") + String(millis()) + String("-") + String(messageCounter++);

  JsonArray readings = doc["readings"].to<JsonArray>();

  JsonObject r1 = readings.add<JsonObject>();
  r1["sensor_key"] = "probe_1";
  r1["value"] = tempProbe1;
  r1["unit"] = "C";

  JsonObject r2 = readings.add<JsonObject>();
  r2["sensor_key"] = "probe_2";
  r2["value"] = tempProbe2;
  r2["unit"] = "C";

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  String resp = http.getString();
  lastPostCode = code;
  lastPostAtMs = millis();
  lastPostResp = resp.length() > 240 ? resp.substring(0, 240) : resp;
  logEvent("POST code: " + String(code));
  logEvent("POST resp: " + lastPostResp);

  http.end();
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(BLUE_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  setBlueLed(false);
  setRedLed(false);

  handleFactoryResetButton();

  snprintf(apiUrl, API_URL_MAX, "%s", DEFAULT_API_URL);
  snprintf(deviceKey, DEVICE_KEY_MAX, "%s", DEFAULT_DEVICE_KEY);
  snprintf(deviceToken, DEVICE_TOKEN_MAX, "%s", DEFAULT_DEVICE_TOKEN);
  snprintf(locationHint, LOCATION_HINT_MAX, "%s", DEFAULT_LOCATION_HINT);

  configStore.begin("coldroom", false);
  String storedApi = configStore.isKey("api_url") ? configStore.getString("api_url") : String(apiUrl);
  String storedKey = configStore.isKey("device_key") ? configStore.getString("device_key") : String(deviceKey);
  String storedToken = configStore.isKey("device_token") ? configStore.getString("device_token") : String(deviceToken);
  String storedLocation = configStore.isKey("location_hint") ? configStore.getString("location_hint") : String(locationHint);
  configStore.end();

  snprintf(apiUrl, API_URL_MAX, "%s", storedApi.c_str());
  snprintf(deviceKey, DEVICE_KEY_MAX, "%s", storedKey.c_str());
  snprintf(deviceToken, DEVICE_TOKEN_MAX, "%s", storedToken.c_str());
  snprintf(locationHint, LOCATION_HINT_MAX, "%s", storedLocation.c_str());

  ds18b20.begin();
  printSensorInventory();

  Serial.print("probe_1 ROM: ");
  Serial.println(addressToString(PROBE_1_ADDR));
  Serial.print("probe_2 ROM: ");
  Serial.println(addressToString(PROBE_2_ADDR));

  if (!ds18b20.isConnected(PROBE_1_ADDR)) {
    Serial.println("Warning: probe_1 ROM not detected on bus.");
  }
  if (!ds18b20.isConnected(PROBE_2_ADDR)) {
    Serial.println("Warning: probe_2 ROM not detected on bus.");
  }

  if (!connectWifi()) {
    logEvent("WiFi not ready. Telemetry will retry in loop.");
  }

  if (!hasDeviceCredentials()) {
    logEvent("No device credentials yet. Starting claim flow.");
    ensureDeviceCredentials();
  }

  ds18b20.requestTemperatures();
  float probe1 = readTempCByAddress(PROBE_1_ADDR);
  float probe2 = readTempCByAddress(PROBE_2_ADDR);
  lastProbe1C = probe1;
  lastProbe2C = probe2;

  if (probe1 == DEVICE_DISCONNECTED_C || probe2 == DEVICE_DISCONNECTED_C) {
    Serial.println("Skipping first send: probe_1 or probe_2 disconnected.");
  } else {
    postReading(probe1, probe2);
  }

  lastSend = millis();
}

void loop() {
  handleFactoryResetButton();
  localServer.handleClient();

  if (!hasDeviceCredentials()) {
    ensureDeviceCredentials();
  }

  if (millis() - lastSend >= sendMs) {
    ds18b20.requestTemperatures();
    float probe1 = readTempCByAddress(PROBE_1_ADDR);
    float probe2 = readTempCByAddress(PROBE_2_ADDR);
    lastProbe1C = probe1;
    lastProbe2C = probe2;

    Serial.print("Probe 1 (C): ");
    Serial.println(probe1);
    Serial.print("Probe 2 (C): ");
    Serial.println(probe2);

    if (probe1 == DEVICE_DISCONNECTED_C || probe2 == DEVICE_DISCONNECTED_C) {
      Serial.println("Skipping send: probe_1 or probe_2 disconnected.");
    } else {
      postReading(probe1, probe2);
    }

    lastSend = millis();
  }
}

#endif

