#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>

const char* WIFI_SSID = "YOUR_WIFI";
const char* WIFI_PASS = "YOUR_PASS";

const char* API_URL = "http://YOUR_PC_IP:3000/api/iot/v1/readings";
const char* DEVICE_KEY = "ESP32-HS6-01";
const char* DEVICE_TOKEN = "PASTE_RAW_TOKEN_FROM_UI";

const int ONE_WIRE_PIN = 4;
const unsigned long SEND_MS = 30000;
unsigned long lastSend = 0;

OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature ds18b20(&oneWire);

void connectWifi() {
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

float readTemperatureC() {
  ds18b20.requestTemperatures();
  float tempC = ds18b20.getTempCByIndex(0);
  return tempC;
}

void postReading(float tempC) {
  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
  }

  HTTPClient http;
  http.begin(API_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("x-device-token", DEVICE_TOKEN);

  JsonDocument doc;
  doc["device_key"] = DEVICE_KEY;
  doc["message_id"] = String("esp32-") + String(millis());
  doc["reading_time"] = "2026-06-02T10:00:00Z";

  JsonArray readings = doc["readings"].to<JsonArray>();
  JsonObject r1 = readings.add<JsonObject>();
  r1["sensor_key"] = "probe_1";
  r1["value"] = tempC;
  r1["unit"] = "C";

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  String resp = http.getString();

  Serial.print("POST code: ");
  Serial.println(code);
  Serial.println(resp);

  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  ds18b20.begin();
  connectWifi();

  float tempC = readTemperatureC();
  if (tempC == DEVICE_DISCONNECTED_C) {
    Serial.println("DS18B20 not detected. Check wiring.");
  } else {
    postReading(tempC);
  }

  lastSend = millis();
}

void loop() {
  if (millis() - lastSend >= SEND_MS) {
    float tempC = readTemperatureC();
    if (tempC == DEVICE_DISCONNECTED_C) {
      Serial.println("Skipping send: DS18B20 disconnected.");
    } else {
      postReading(tempC);
    }
    lastSend = millis();
  }
}
