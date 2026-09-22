#!/usr/bin/env pwsh
<#
.SYNOPSIS
  从 backups/<时间戳>/ 目录还原固件到 ESP32-S3.
.PARAMETER BackupDir
  含 bootloader.bin / partitions.bin / app.bin / model.bin 的目录.
.PARAMETER Port
  串口名, 默认自动检测.
.EXAMPLE
  ls backups/                                  # 看有哪些备份
  .\restore-firmware.ps1 -BackupDir backups/2026-08-29-1530
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory=$true)][string]$BackupDir,
  [string]$Port = ""
)

$ErrorActionPreference = "Stop"
chcp 65001 | Out-Null

if (-not (Test-Path $BackupDir)) {
  Write-Error "Backup dir not found: $BackupDir"; exit 1
}
foreach ($f in "bootloader.bin","partitions.bin","app.bin","model.bin") {
  if (-not (Test-Path (Join-Path $BackupDir $f))) {
    Write-Error "Missing file: $f in $BackupDir"; exit 1
  }
}

$esptool = "esptool.py"
if (-not $Port) {
  if ($env:OS -match "Windows") { $Port = "COM4" } else { $Port = "/dev/ttyUSB0" }
}

Write-Host "=== Restore Firmware ===" -ForegroundColor Cyan
Write-Host "From: $BackupDir"
Write-Host "Port: $Port"
Write-Host ""

# 写回 4 段
& $esptool --chip esp32s3 --port $Port --baud 921600 `
  --before default-reset --after no-reset `
  write-flash 0x0000 (Join-Path $BackupDir "bootloader.bin")

& $esptool --chip esp32s3 --port $Port --baud 921600 `
  --before default-reset --after no-reset `
  write-flash 0x8000 (Join-Path $BackupDir "partitions.bin")

& $esptool --chip esp32s3 --port $Port --baud 921600 `
  --before default-reset --after no-reset `
  write-flash 0x10000 (Join-Path $BackupDir "app.bin")

# model 偏移取决于 layout: 检查 partitions.bin 第一行 (md5 之后) 的 model 起始
$modelOffset = "0x150000"  # 默认 web-chat 布局
# 用户可手动 -ModelOffset 覆盖

if ($PSBoundParameters.ContainsKey('ModelOffset')) {
  $modelOffset = $ModelOffset
}
Write-Host "Model @ $modelOffset ..." -ForegroundColor Yellow
& $esptool --chip esp32s3 --port $Port --baud 921600 `
  --before default-reset --after hard-reset `
  write-flash $modelOffset (Join-Path $BackupDir "model.bin")

Write-Host ""
Write-Host "✓ 还原完成!" -ForegroundColor Green
Write-Host "拔 USB 等 2 秒再插" -ForegroundColor Magenta
