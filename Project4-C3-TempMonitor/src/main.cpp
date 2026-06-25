#include <OneWire.h>
#include <DallasTemperature.h>

#ifndef SENSOR_ID_SCAN_ONLY
#define SENSOR_ID_SCAN_ONLY 0
#endif

// DS18B20 data line. Needs a 4.7k pull-up between DATA and 3V3.
// GPIO4 is a safe (non-strapping) pin on the ESP32-C3.
const int ONE_WIRE_PIN = 4;

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

#if SENSOR_ID_SCAN_ONLY

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("ESP32-C3 DS18B20 scan mode");
  Serial.print("OneWire pin: GPIO");
  Serial.println(ONE_WIRE_PIN);

  ds18b20.begin();
  printSensorInventory();

  if (ds18b20.getDeviceCount() < 1) {
    Serial.println("Warning: no DS18B20 found on the bus (check wiring / 4.7k pull-up).");
  }
}

void loop() {
  ds18b20.requestTemperatures();

  uint8_t sensorCount = ds18b20.getDeviceCount();
  for (uint8_t i = 0; i < sensorCount; i++) {
    Serial.print("Sensor ");
    Serial.print(i);
    Serial.print(" temp C: ");
    Serial.println(ds18b20.getTempCByIndex(i));
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
// per build via platformio_override.ini (gitignored). These committed defaults
// are non-secret placeholders only and must be overridden before deployment.
#ifndef BBH_LOCAL_USERNAME
#define BBH_LOCAL_USERNAME "admin"
#endif

#ifndef BBH_LOCAL_PASSWORD
#define BBH_LOCAL_PASSWORD "changeme"
#endif

#ifndef BBH_SETUP_AP_PASSWORD
#define BBH_SETUP_AP_PASSWORD "bbh-setup"
#endif

// Single DS18B20 read by bus index 0 (no hard-coded ROM address, so any probe
// works for this test build).
class TempSensorAdapter : public BbhSensorAdapter {
 public:
  void begin() override {
    ds18b20.begin();
    printSensorInventory();
    if (ds18b20.getDeviceCount() < 1) {
      Serial.println("Warning: no DS18B20 found on the bus (check wiring / 4.7k pull-up).");
    }
  }

  void describeSensors(BbhSensorDefinition* definitions, size_t maxCount, size_t& outCount) override {
    outCount = 0;
    if (maxCount < 1) {
      return;
    }
    definitions[0] = {"probe_1", "temperature", "C"};
    outCount = 1;
  }

  void readTelemetry(BbhTelemetryReading* readings, size_t maxCount, size_t& outCount) override {
    outCount = 0;
    if (maxCount < 1) {
      return;
    }

    ds18b20.requestTemperatures();
    float probe1 = ds18b20.getTempCByIndex(0);

    Serial.print("Probe 1 (C): ");
    Serial.println(probe1);

    readings[0] = {"probe_1", probe1, "C", probe1 != DEVICE_DISCONNECTED_C};
    outCount = 1;
  }
};

const BbhFirmwareCoreConfig kFirmwareConfig = {
    "c3-temp-1.0.0",
    "C3TEMP-SETUP",
    {
        BBH_BOOTSTRAP_BROKER_HOST,
        BBH_BOOTSTRAP_BROKER_PORT,
        BBH_BOOTSTRAP_USERNAME,
        BBH_BOOTSTRAP_PASSWORD,
        BBH_BOOTSTRAP_CLIENT_ID,
        BBH_BOOTSTRAP_CLAIM_TOPIC,
        BBH_BOOTSTRAP_REPLY_PREFIX,
        "Test Bench",
    },
    {
        BBH_LOCAL_USERNAME,
        BBH_LOCAL_PASSWORD,
        BBH_SETUP_AP_PASSWORD,
    },
    {
        // ESP32-C3 pins: status LEDs on free GPIOs (wire if you want them),
        // factory-reset button on the onboard BOOT button (GPIO9, active-low).
        5,   // blueLedPin
        6,   // redLedPin
        9,   // resetButtonPin (BOOT)
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

TempSensorAdapter sensorAdapter;
BbhIotFirmwareCore firmwareCore(kFirmwareConfig, sensorAdapter);

void setup() {
  firmwareCore.setup();
}

void loop() {
  firmwareCore.loop();
}

#endif
