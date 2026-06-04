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

$envName = $Board
$ini = @"
[env:$envName]
platform = $Platform
board = $Board
framework = $Framework
monitor_speed = 115200
"@

Set-Content -Path (Join-Path $projectPath "platformio.ini") -Value $ini -Encoding UTF8

$mainCpp = @"
#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("$Name started");
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

## Protocol Baseline Requirement

This project must follow `Project3-ColdRoom-TempMonitor` protocol baseline unless this file documents an approved deviation.

Reference docs:

- `FIRMWARE_BASELINE.md`
- `.github/copilot-instructions.md`

## Required Contract Checklist

- [ ] Telemetry endpoint path uses `/api/iot/v1/readings` (current backend port `5010`).
- [ ] Claim flow uses `/api/iot/v1/device-claims` create + poll lifecycle.
- [ ] Claim statuses handled: `pending`, `approved`, `expired`, `rejected`, `claim_not_found`.
- [ ] Telemetry includes headers: `Content-Type`, `Accept`, `x-device-token`.
- [ ] Payload includes `device_key`, unique `message_id`, and `readings[]`.
- [ ] Sensor keys are deterministic (for probes: `probe_1`, `probe_2`) and units are canonical (`C`, `%`, `pH`, `mS/cm`).
- [ ] Approved credentials are persisted and restored after reboot.
- [ ] Local diagnostics UI on port `80` with auth (`/` and `/api/status`).
- [ ] LED behavior: red blink connecting, blue success, red solid failure timeout.
- [ ] Factory reset long-press is non-blocking and clears Wi-Fi + persisted credentials.

## Verification Before Production

- [ ] Captive portal onboarding works and reconnects after Wi-Fi loss.
- [ ] Claim approval persists credentials across restart.
- [ ] Telemetry returns success and backend reports `saved_count` updates.
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
    defaultEnv = $envName
  }
  $registry.projects += $entry
}

$registry | ConvertTo-Json -Depth 5 | Set-Content -Path $registryPath -Encoding UTF8

Write-Host "Created project: $Name"
Write-Host "Path: $projectPath"
