param(
  [string]$ArduinoCli = "arduino-cli",
  [string]$EspBoard = "esp8266:esp8266:generic"
)
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$stage = Join-Path $repo ".purge-build"
$uno = Join-Path $stage "airtankpurge"
$esp = Join-Path $stage "esp01"
New-Item -ItemType Directory -Force $uno, $esp | Out-Null
Copy-Item -LiteralPath (Join-Path $repo "airtankpurge.ino") -Destination $uno -Force
# Shared headers must retain their relative layout for the Uno.
$headers = Join-Path $uno "purge_telemetry"
New-Item -ItemType Directory -Force (Join-Path $headers "esp01") | Out-Null
foreach ($name in @("UnoTelemetry.h", "ControllerClock.h", "DurationSettings.h")) {
  Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $headers -Force
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "esp01/Protocol.h") -Destination (Join-Path $headers "esp01") -Force
foreach ($name in @("esp01.ino", "Protocol.h", "config.h")) {
  Copy-Item -LiteralPath (Join-Path $PSScriptRoot "esp01/$name") -Destination $esp -Force
}
& $ArduinoCli compile --fqbn arduino:avr:uno $uno
if ($LASTEXITCODE -ne 0) { throw "Uno build failed" }
& $ArduinoCli compile --fqbn $EspBoard $esp
if ($LASTEXITCODE -ne 0) { throw "ESP build failed" }
