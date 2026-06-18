#include <OneWire.h>
#include <DallasTemperature.h>

#ifndef SENSOR_ID_SCAN_ONLY
#define SENSOR_ID_SCAN_ONLY 0
#endif

const int ONE_WIRE_PIN = 4;

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
  return ds18b20.getTempCByIndex(index);
}

float readTempCByAddress(const DeviceAddress address) {
  return ds18b20.getTempC(address);
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
// per build via platformio_override.ini (gitignored). These committed defaults are
// non-secret placeholders only and must be overridden before deployment.
#ifndef BBH_LOCAL_USERNAME
#define BBH_LOCAL_USERNAME "admin"
#endif

#ifndef BBH_LOCAL_PASSWORD
#define BBH_LOCAL_PASSWORD "changeme"
#endif

#ifndef BBH_SETUP_AP_PASSWORD
#define BBH_SETUP_AP_PASSWORD "bbh-setup"
#endif

class ColdRoomSensorAdapter : public BbhSensorAdapter {
 public:
  void begin() override {
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
  }

  void describeSensors(BbhSensorDefinition* definitions, size_t maxCount, size_t& outCount) override {
    outCount = 0;
    if (maxCount < 2) {
      return;
    }

    definitions[0] = {"probe_1", "temperature", "C"};
    definitions[1] = {"probe_2", "temperature", "C"};
    outCount = 2;
  }

  void readTelemetry(BbhTelemetryReading* readings, size_t maxCount, size_t& outCount) override {
    outCount = 0;
    if (maxCount < 2) {
      return;
    }

    ds18b20.requestTemperatures();

    float probe1 = readTempCByAddress(PROBE_1_ADDR);
    float probe2 = readTempCByAddress(PROBE_2_ADDR);

    Serial.print("Probe 1 (C): ");
    Serial.println(probe1);
    Serial.print("Probe 2 (C): ");
    Serial.println(probe2);

    readings[0] = {"probe_1", probe1, "C", probe1 != DEVICE_DISCONNECTED_C};
    readings[1] = {"probe_2", probe2, "C", probe2 != DEVICE_DISCONNECTED_C};
    outCount = 2;
  }
};

const BbhFirmwareCoreConfig kFirmwareConfig = {
    "coldroom-temp-1.0.0",
    "COLDROOM-SETUP",
    {
        BBH_BOOTSTRAP_BROKER_HOST,
        BBH_BOOTSTRAP_BROKER_PORT,
        BBH_BOOTSTRAP_USERNAME,
        BBH_BOOTSTRAP_PASSWORD,
        BBH_BOOTSTRAP_CLIENT_ID,
        BBH_BOOTSTRAP_CLAIM_TOPIC,
        BBH_BOOTSTRAP_REPLY_PREFIX,
        "Coldroom North",
    },
    {
        BBH_LOCAL_USERNAME,
        BBH_LOCAL_PASSWORD,
        BBH_SETUP_AP_PASSWORD,
    },
    {
        2,
        15,
        0,
    },
    {
        30000UL,
        60000UL,
        20000UL,
        5000UL,
        30000UL,
        5000UL,
        250UL,
    },
};

ColdRoomSensorAdapter sensorAdapter;
BbhIotFirmwareCore firmwareCore(kFirmwareConfig, sensorAdapter);

void setup() {
  firmwareCore.setup();
}

void loop() {
  firmwareCore.loop();
}

#endif
