#pragma once
//
// BbhPreferences8266 — a drop-in subset of the ESP32 `Preferences` API for the
// ESP8266, backed by a small JSON file in LittleFS. It exists so the shared
// BBHIotFirmwareCore can persist config identically on both chip families
// without any #ifdef in the core logic (see platform_compat.h, which aliases
// `BbhPreferences` to the real Preferences on ESP32 and to this on ESP8266).
//
// Only the methods the core actually uses are implemented:
//   begin/end, clear, remove,
//   getString/putString, getUShort/putUShort, getUChar/putUChar,
//   getUInt/putUInt.
//
// Storage model: one file per namespace at /nvs_<name>.json holding a flat JSON
// object of key -> value. The whole object is loaded on begin() and rewritten
// on every mutation (config is tiny and writes are rare, so this is simpler and
// safer than incremental updates). ESP32 NVS caps keys at 15 chars; keep keys
// within that limit so both backends behave identically.
//
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

class BbhPreferences8266 {
 public:
  // readOnly is accepted for API-compatibility; this shim always allows writes.
  bool begin(const char* name, bool /*readOnly*/ = false) {
    path_ = String("/nvs_") + name + ".json";
    doc_.clear();
    open_ = true;
    if (!LittleFS.exists(path_)) {
      return true;  // fresh namespace, empty document
    }
    File f = LittleFS.open(path_, "r");
    if (!f) {
      return true;
    }
    DeserializationError err = deserializeJson(doc_, f);
    f.close();
    if (err) {
      doc_.clear();  // corrupt store -> start clean rather than wedge boot
    }
    return true;
  }

  void end() {
    open_ = false;
    doc_.clear();
  }

  bool clear() {
    doc_.clear();
    return flush();
  }

  bool remove(const char* key) {
    doc_.remove(key);
    return flush();
  }

  size_t putString(const char* key, const String& value) {
    doc_[key] = value;
    flush();
    return value.length();
  }

  size_t putUShort(const char* key, uint16_t value) {
    doc_[key] = value;
    flush();
    return sizeof(uint16_t);
  }

  size_t putUChar(const char* key, uint8_t value) {
    doc_[key] = value;
    flush();
    return sizeof(uint8_t);
  }

  size_t putUInt(const char* key, uint32_t value) {
    doc_[key] = value;
    flush();
    return sizeof(uint32_t);
  }

  String getString(const char* key, const String& defaultValue = String()) {
    if (!doc_[key].is<const char*>() && !doc_[key].is<String>()) {
      return defaultValue;
    }
    return doc_[key].as<String>();
  }

  uint16_t getUShort(const char* key, uint16_t defaultValue = 0) {
    if (!doc_[key].is<uint16_t>() && !doc_[key].is<int>()) {
      return defaultValue;
    }
    return doc_[key].as<uint16_t>();
  }

  uint8_t getUChar(const char* key, uint8_t defaultValue = 0) {
    if (!doc_[key].is<uint8_t>() && !doc_[key].is<int>()) {
      return defaultValue;
    }
    return doc_[key].as<uint8_t>();
  }

  uint32_t getUInt(const char* key, uint32_t defaultValue = 0) {
    if (!doc_[key].is<uint32_t>() && !doc_[key].is<int>()) {
      return defaultValue;
    }
    return doc_[key].as<uint32_t>();
  }

 private:
  bool flush() {
    if (!open_) {
      return false;
    }
    File f = LittleFS.open(path_, "w");
    if (!f) {
      return false;
    }
    serializeJson(doc_, f);
    f.close();
    return true;
  }

  String path_;
  JsonDocument doc_;
  bool open_ = false;
};
