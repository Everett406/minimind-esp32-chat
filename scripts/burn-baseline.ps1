#!/usr/bin/env pwsh
<#
.SYNOPSIS
  烧录基线版 (1.04MB 串口 chat, 无 Web) 固件到 ESP32-S3.
.NOTES
  基线版用旧 partition 布局:
    - factory @ 0x10000 size 0x110000 (1.06 MB)
    - model   @ 0x120000 size 0xED0000 (14.5 MB)
  所以烧 model 时地址是 0x120000 (不是 0x150000).
#>
[CmdletBinding()]
param([string]$Port = "")

$ErrorActionPreference = "Stop"
chcp 65001 | Out-Null

$repo = Split-Path -Parent $PSScriptRoot
$frmDir = Join-Path $repo "firmware\baseline-serial-chat"
$esptool = "esptool.py"

if (-not $Port) {
  if ($env:OS -match "Windows") { $Port = "COM4" } else { $Port = "/dev/ttyUSB0" }
}

Write-Host "=== MiniMind ESP32-S3 Baseline (Serial Chat) Flash ===" -ForegroundColor Cyan
Write-Host "Port: $Port"
Write-Host ""

# 1. bootloader
& $esptool --chip esp32s3 --port $Port --baud 921600 `
  --before default-reset --after no-reset `
  write-flash 0x0000 (Join-Path $frmDir "bootloader.bin")

# 2. partitions
& $esptool --chip esp32s3 --port $Port --baud 921600 `
  --before default-reset --after no-reset `
  write-flash 0x8000 (Join-Path $frmDir "partitions.bin")

# 3. app
& $esptool --chip esp32s3 --port $Port --baud 921600 `
  --before default-reset --after no-reset `
  write-flash 0x10000 (Join-Path $frmDir "app.bin")

# 4. model @ 0x120000 (基线版用旧偏移!)
& $esptool --chip esp32s3 --port $Port --baud 921600 `
  --before default-reset --after hard-reset `
  write-flash 0x120000 (Join-Path $frmDir "model.bin")

Write-Host ""
Write-Host "✓ 烧录完成!" -ForegroundColor Green
Write-Host "拔 USB 等 2 秒再插, 串口监视器 (115200) 看输出" -ForegroundColor Magenta
