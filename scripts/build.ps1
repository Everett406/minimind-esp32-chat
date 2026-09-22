#!/usr/bin/env pwsh
<#
.SYNOPSIS
  从 source/ 用 arduino-cli 编译 (PowerShell 版).
.EXAMPLE
  .\build.ps1
  .\build.ps1 -BuildDir D:\build
#>
[CmdletBinding()]
param([string]$BuildDir = "")

$ErrorActionPreference = "Stop"
chcp 65001 | Out-Null

$repo = Split-Path -Parent $PSScriptRoot
$src  = Join-Path $repo "source"
if (-not $BuildDir) { $BuildDir = Join-Path $repo "build" }

$FQBN = "esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=default,CDCOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=dio,FlashSize=16M,PartitionScheme=custom,PSRAM=opi,DebugLevel=none"

if (-not (Get-Command arduino-cli -ErrorAction SilentlyContinue)) {
  Write-Error "arduino-cli not found. Install: https://arduino.github.io/arduino-cli/"
  exit 1
}

Write-Host "=== Build source/ with arduino-cli ===" -ForegroundColor Cyan
Write-Host "FQBN: $FQBN"
Write-Host "Build dir: $BuildDir"
Write-Host ""

arduino-cli compile --fqbn $FQBN --build-path $BuildDir $src

Write-Host ""
Write-Host "✓ 编译完成!" -ForegroundColor Green
Get-ChildItem (Join-Path $BuildDir "YuanDiArduino.ino.bin") -ErrorAction SilentlyContinue | Format-Table Name, Length
