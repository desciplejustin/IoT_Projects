#include <Arduino.h>
#include <WiFi.h>
#include <MQTT.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>

const char* WIFI_SSID = "YOUR_WIFI";
const char* WIFI_PASS = "YOUR_PASS";

const char* MQTT_HOST = "192.168.1.10";
const int MQTT_PORT = 1883;
const char* MQTT_USER = "device-user";
const char* MQTT_PASS = "device-pass";
const char* TOPIC_ROOT = "bbh/iot";
// device_key must match an active row in iot_devices on the backend, or telemetry
// is rejected. Set this to the smoke-test device you registered, not a copy-paste.
const char* DEVICE_CODE = "coldroom-smoketest-001";
const char* FIRMWARE_VERSION = "2.0.0";

const int ONE_WIRE_PIN = 4;
const unsigned long TELEMETRY_INTERVAL_MS = 30000;
const unsigned long STATUS_INTERVAL_MS = 60000;

OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature ds18b20(&oneWire);
WiFiClient net;
MQTTClient mqtt(1024);

unsigned long lastTelemetryAt = 0;
unsigned long lastStatusAt = 0;
uint32_t messageCounter = 0;

String deviceTopic(const char* suffix) {
  return String(TOPIC_ROOT) + "/" + DEVICE_CODE + "/" + suffix;
}

String mqttClientId() {
  uint64_t chip = ESP.getEfuseMac();
  char out[40];
  snprintf(out, sizeof(out), "%s-%08X", DEVICE_CODE, (uint32_t)chip);
  return String(out);
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());
}

bool connectMqtt() {
  if (mqtt.connected()) {
    return true;
  }

  String stateTopic = deviceTopic("state");
  mqtt.setWill(stateTopic.c_str(), "offline", true, 1);

  Serial.print("MQTT connecting to ");
  Serial.print(MQTT_HOST);
  Serial.print(":");
  Serial.println(MQTT_PORT);

  if (!mqtt.connect(mqttClientId().c_str(), MQTT_USER, MQTT_PASS)) {
    Serial.println("MQTT connect failed");
    return false;
  }

  mqtt.publish(stateTopic, "online", true, 1);
  Serial.println("MQTT connected");
  return true;
}

float readTemperatureC() {
  ds18b20.requestTemperatures();
  return ds18b20.getTempCByIndex(0);
}

void publishStatus() {
  JsonDocument doc;
  doc["device_code"] = DEVICE_CODE;
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["uptime_seconds"] = millis() / 1000UL;
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["ip_address"] = WiFi.localIP().toString();
  doc["status"] = "online";

  String payload;
  serializeJson(doc, payload);
  mqtt.publish(deviceTopic("status"), payload, false, 1);
}

void publishTelemetry(float tempC) {
  JsonDocument doc;
  doc["schema_version"] = 1;
  doc["device_code"] = DEVICE_CODE;
  doc["message_id"] = String(DEVICE_CODE) + "-" + String(millis()) + "-" + String(messageCounter++);
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["uptime_seconds"] = millis() / 1000UL;

  // Canonical reading field is sensor_key (see CONTRACT.md). The backend takes the
  // reading type from the registered sensor row, so reading_type is not sent here.
  JsonArray readings = doc["readings"].to<JsonArray>();
  JsonObject reading = readings.add<JsonObject>();
  reading["sensor_key"] = "probe_1";
  reading["value"] = tempC;
  reading["unit"] = "C";

  String payload;
  serializeJson(doc, payload);
  mqtt.publish(deviceTopic("telemetry"), payload, false, 1);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Project1 MQTT smoke test");

  ds18b20.begin();
  connectWifi();

  mqtt.begin(MQTT_HOST, MQTT_PORT, net);
  connectMqtt();

  float tempC = readTemperatureC();
  if (tempC == DEVICE_DISCONNECTED_C) {
    Serial.println("DS18B20 not detected. Check wiring.");
  } else if (mqtt.connected()) {
    publishStatus();
    publishTelemetry(tempC);
  }

  lastTelemetryAt = millis();
  lastStatusAt = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
  }

  if (!mqtt.connected()) {
    connectMqtt();
  }

  mqtt.loop();

  if (millis() - lastStatusAt >= STATUS_INTERVAL_MS && mqtt.connected()) {
    publishStatus();
    lastStatusAt = millis();
  }

  if (millis() - lastTelemetryAt >= TELEMETRY_INTERVAL_MS) {
    float tempC = readTemperatureC();
    if (tempC == DEVICE_DISCONNECTED_C) {
      Serial.println("Skipping telemetry: DS18B20 disconnected.");
    } else if (mqtt.connected()) {
      publishTelemetry(tempC);
      Serial.print("Published temperature C: ");
      Serial.println(tempC);
    }
    lastTelemetryAt = millis();
  }
}
