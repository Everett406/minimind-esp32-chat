#!/usr/bin/env pwsh
<#
.SYNOPSIS
  烧录 Web Chat 版固件到 ESP32-S3 (元旦 N16R8 板).
.DESCRIPTION
  按顺序烧 3 段: bootloader + partitions + app + model.
  自动检测串口, 默认 COM4 (Windows) / /dev/ttyUSB0 (Linux).
.PARAMETER Port
  串口名, 默认自动检测.
.EXAMPLE
  .\burn-web-chat.ps1
  .\burn-web-chat.ps1 -Port COM3
#>
[CmdletBinding()]
param(
  [string]$Port = ""
)

$ErrorActionPreference = "Stop"
chcp 65001 | Out-Null

$repo = Split-Path -Parent $PSScriptRoot
$frmDir = Join-Path $repo "firmware\web-chat"
$esptool = "esptool.py"

# 自动找串口
if (-not $Port) {
  if ($IsWindows -or $env:OS -match "Windows") {
    $Port = "COM4"
  } else {
    $Port = "/dev/ttyUSB0"
  }
}

Write-Host "=== MiniMind ESP32-S3 Web Chat Flash ===" -ForegroundColor Cyan
Write-Host "Port: $Port"
Write-Host "Firmware: $frmDir"
Write-Host ""

# 1. bootloader
Write-Host "[1/4] bootloader @ 0x0000 ..." -ForegroundColor Yellow
& $esptool --chip esp32s3 --port $Port --baud 921600 `
  --before default-reset --after no-reset `
  write-flash 0x0000 (Join-Path $frmDir "bootloader.bin")

# 2. partition table
Write-Host "[2/4] partitions @ 0x8000 ..." -ForegroundColor Yellow
& $esptool --chip esp32s3 --port $Port --baud 921600 `
  --before default-reset --after no-reset `
  write-flash 0x8000 (Join-Path $frmDir "partitions.bin")

# 3. app
Write-Host "[3/4] app @ 0x10000 (~12s) ..." -ForegroundColor Yellow
& $esptool --chip esp32s3 --port $Port --baud 921600 `
  --before default-reset --after no-reset `
  write-flash 0x10000 (Join-Path $frmDir "app.bin")

# 4. model (这一步最慢, 40-160s)
Write-Host "[4/4] model @ 0x150000 (~40-160s) ..." -ForegroundColor Yellow
& $esptool --chip esp32s3 --port $Port --baud 921600 `
  --before default-reset --after hard-reset `
  write-flash 0x150000 (Join-Path $frmDir "model.bin")

Write-Host ""
Write-Host "✓ 烧录完成!" -ForegroundColor Green
Write-Host ""
Write-Host "下一步: 拔 USB 等 2 秒再插 (CH343 DTR reset 不可靠)" -ForegroundColor Magenta
Write-Host "        连 WiFi 'YuanDi-S3-MiniMind' (无密码)" -ForegroundColor Magenta
Write-Host "        浏览器开 http://192.168.4.1" -ForegroundColor Magenta
