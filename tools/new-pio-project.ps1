param(
  [Parameter(Mandatory = $true)]
  [string]$Name,

  [Parameter(Mandatory = $true)]
  [string]$Board,

  [string]$Platform = "espressif32",
  [string]$Framework = "arduino",
  [string]$Description = "New PlatformIO project"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$projectPath = Join-Path $repoRoot $Name

if (Test-Path $projectPath) {
  throw "Project path already exists: $projectPath"
}

New-Item -ItemType Directory -Path (Join-Path $projectPath "src") -Force | Out-Null

$ini = @"
[platformio]
default_envs = app

[env]
platform = $Platform
board = $Board
framework = $Framework
monitor_speed = 115200

[env:app]
extends = env
lib_deps =
  bblanchon/ArduinoJson @ ^7
  256dpi/MQTT @ ^2.5.2
  tzapu/WiFiManager @ ^2.0.17
"@

Set-Content -Path (Join-Path $projectPath "platformio.ini") -Value $ini -Encoding UTF8

$mainCpp = @"
#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("$Name started");
  Serial.println("TODO: implement MQTT telemetry baseline from FIRMWARE_BASELINE.md");
}

void loop() {
  delay(1000);
  Serial.println("heartbeat");
}
"@

Set-Content -Path (Join-Path $projectPath "src\main.cpp") -Value $mainCpp -Encoding UTF8

$projectMd = @"
# $Name

## Purpose

$Description

## Hardware

- Board: $Board
- Platform: $Platform
- Framework: $Framework

## Current Status

Starter project scaffold created.

## Device-Specific Scope

- Sensor type: TODO
- Sensor count: TODO
- Sensor mapping: TODO
- Board profile: $Board
- Primary use: TODO
- Supported control topics: TODO

## MQTT Baseline Requirement

This project must follow the MQTT-first firmware contract unless this file documents an approved deviation.

Reference docs:

- `FIRMWARE_BASELINE.md`
- `iot_mqtt_integration_plan.md`
- `.github/copilot-instructions.md`

## Required Contract Checklist

- [ ] Telemetry publishes to `{topic_root}/{device_code}/telemetry`.
- [ ] Heartbeat publishes to `{topic_root}/{device_code}/status`.
- [ ] Retained device availability publishes to `{topic_root}/{device_code}/state`.
- [ ] Runtime settings persist Wi-Fi and MQTT config locally.
- [ ] Telemetry includes `schema_version`, `device_code`, unique `message_id`, and `readings[]`.
- [ ] Sensor codes are deterministic and units are canonical (`C`, `%`, `pH`, `mS/cm`).
- [ ] Device reconnects after Wi-Fi or broker loss.
- [ ] Local diagnostics UI on port `80` is present for baseline telemetry nodes.
- [ ] LED behavior matches the workspace baseline.
- [ ] Factory reset clears Wi-Fi and stored device config.

## Verification Before Production

- [ ] Captive portal onboarding works and reconnects after Wi-Fi loss.
- [ ] Persisted MQTT settings survive restart.
- [ ] Broker receives telemetry, status, and retained state updates.
- [ ] Local dashboard shows live values and recent logs.
"@

Set-Content -Path (Join-Path $projectPath "PROJECT.md") -Value $projectMd -Encoding UTF8

$registryPath = Join-Path $repoRoot "projects.registry.json"
if (Test-Path $registryPath) {
  $registry = Get-Content -Path $registryPath -Raw | ConvertFrom-Json
} else {
  $registry = [pscustomobject]@{
    version = 1
    projects = @()
  }
}

$existing = $registry.projects | Where-Object { $_.name -eq $Name }
if (-not $existing) {
  $entry = [pscustomobject]@{
    name = $Name
    path = $Name
    description = $Description
    board = $Board
    platform = $Platform
    framework = $Framework
    defaultEnv = "app"
  }
  $registry.projects += $entry
}

$registry | ConvertTo-Json -Depth 5 | Set-Content -Path $registryPath -Encoding UTF8

Write-Host "Created project: $Name"
Write-Host "Path: $projectPath"
Write-Host "Reminder: add VS Code tasks for the new project in .vscode/tasks.json"
