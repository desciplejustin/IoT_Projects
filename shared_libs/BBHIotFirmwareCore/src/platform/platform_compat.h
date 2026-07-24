#pragma once
//
// platform_compat.h — the ONE place chip-family differences live.
//
// BBHIotFirmwareCore is a single, shared, chip-agnostic body of logic
// (bootstrap/claim, MQTT, telemetry buffering, diagnostics UI, state machine).
// Everything that genuinely differs between ESP32 and ESP8266 — persistence,
// the local web server class, WiFi TX-power/sleep control, the factory MAC, and
// filesystem mount — is aliased or wrapped here and selected at COMPILE TIME by
// the Arduino core's ARDUINO_ARCH_* define.
//
// Consequence (the design goal): a change to core logic benefits EVERY board
// automatically, and adding a new chip family means adding one branch here plus
// a backend shim — never forking the core. Keep the core free of #ifdef; if you
// reach for a platform macro in BBHIotFirmwareCore.cpp, add a wrapper here
// instead.
//
#include <Arduino.h>
#include <LittleFS.h>

#if defined(ARDUINO_ARCH_ESP32)

#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

using BbhPreferences = Preferences;
using BbhWebServer = WebServer;

// Factory MAC as 6 bytes, most-significant first. Preserves the exact byte
// order the fleet's ESP32 devices already derive their hardware_id from — do
// not "simplify" this to WiFi.macAddress() on ESP32 or every device re-onboards.
inline void bbhPlatformMacBytes(uint8_t out[6]) {
  uint64_t mac = ESP.getEfuseMac();
  out[0] = static_cast<uint8_t>(mac >> 40);
  out[1] = static_cast<uint8_t>(mac >> 32);
  out[2] = static_cast<uint8_t>(mac >> 24);
  out[3] = static_cast<uint8_t>(mac >> 16);
  out[4] = static_cast<uint8_t>(mac >> 8);
  out[5] = static_cast<uint8_t>(mac);
}

// Whole-dBm TX power. wifi_power_t is in 0.25 dBm units, so scale by 4;
// esp_wifi_set_max_tx_power clamps to the nearest supported bracket.
inline void bbhPlatformSetTxPowerDbm(uint8_t dbm) {
  WiFi.setTxPower(static_cast<wifi_power_t>(dbm * 4));
}

// Low-power profile for the captive setup AP (~8.5 dBm) — the operator is right
// next to the device, and full power browns out the SoftAP on USB power.
inline void bbhPlatformSetTxPowerApLow() {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
}

// Disable modem sleep for stabler RSSI / faster reconnects (mains-powered only).
inline void bbhPlatformDisableModemSleep() {
  WiFi.setSleep(false);
}

// Mount LittleFS, formatting on first use / mount failure.
inline bool bbhPlatformFsBegin() {
  return LittleFS.begin(true);
}

#elif defined(ARDUINO_ARCH_ESP8266)

#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>

#include "BbhPreferences8266.h"

using BbhPreferences = BbhPreferences8266;
using BbhWebServer = ESP8266WebServer;

// ESP8266 has no eFuse MAC API; the station MAC is the stable per-device id.
inline void bbhPlatformMacBytes(uint8_t out[6]) {
  WiFi.macAddress(out);
}

// ESP8266 sets TX power directly in dBm (0.0–20.5).
inline void bbhPlatformSetTxPowerDbm(uint8_t dbm) {
  WiFi.setOutputPower(static_cast<float>(dbm));
}

inline void bbhPlatformSetTxPowerApLow() {
  WiFi.setOutputPower(8.5f);
}

inline void bbhPlatformDisableModemSleep() {
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
}

// ESP8266 LittleFS.begin() does not auto-format; format once on mount failure.
inline bool bbhPlatformFsBegin() {
  if (LittleFS.begin()) {
    return true;
  }
  if (!LittleFS.format()) {
    return false;
  }
  return LittleFS.begin();
}

#else
#error "BBHIotFirmwareCore: unsupported architecture (need ARDUINO_ARCH_ESP32 or ARDUINO_ARCH_ESP8266)"
#endif
