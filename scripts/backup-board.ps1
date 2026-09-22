#!/usr/bin/env pwsh
<#
.SYNOPSIS
  备份当前 ESP32-S3 flash 全部 4 段 (bootloader + partitions + app + model) 到本地.
.DESCRIPTION
  输出到 release/backups/<时间戳>/, 总共 ~15 MB.
  默认从 web-chat 布局读 (model @ 0x150000). 如要备份基线版布局, 用 -Baseline.
#>
[CmdletBinding()]
param(
  [string]$Port = "",
  [switch]$Baseline
)

$ErrorActionPreference = "Stop"
chcp 65001 | Out-Null

$repo = Split-Path -Parent $PSScriptRoot
$backupRoot = Join-Path $repo "backups"
$stamp = Get-Date -Format "yyyy-MM-dd-HHmm"
$dest = Join-Path $backupRoot $stamp
New-Item -Path $dest -ItemType Directory -Force | Out-Null

$esptool = "esptool.py"
if (-not $Port) {
  if ($env:OS -match "Windows") { $Port = "COM4" } else { $Port = "/dev/ttyUSB0" }
}

# 默认 web-chat 布局: model @ 0x150000 size 0xEA0000
$modelOffset = if ($Baseline) { "0x120000" } else { "0x150000" }
$modelSize   = if ($Baseline) { "0xED0000" } else { "0xEA0000" }

Write-Host "=== Backup ESP32-S3 Flash ===" -ForegroundColor Cyan
Write-Host "Port: $Port"
Write-Host "Output: $dest"
Write-Host "Layout: $(if ($Baseline) { 'baseline (model @ 0x120000)' } else { 'web-chat (model @ 0x150000)' })"
Write-Host ""

Write-Host "[1/4] bootloader @ 0x0000 (24 KB) ..." -ForegroundColor Yellow
& $esptool --chip esp32s3 --port $Port --baud 921600 `
  --before default-reset --after no-reset `
  read-flash 0x0000 0x6000 (Join-Path $dest "bootloader.bin")

Write-Host "[2/4] partitions @ 0x8000 (4 KB) ..." -ForegroundColor Yellow
& $esptool --chip esp32s3 --port $Port --baud 921600 `
  --before default-reset --after no-reset `
  read-flash 0x8000 0x1000 (Join-Path $dest "partitions.bin")

Write-Host "[3/4] app @ 0x10000 (1.3 MB) ..." -ForegroundColor Yellow
& $esptool --chip esp32s3 --port $Port --baud 921600 `
  --before default-reset --after no-reset `
  read-flash 0x10000 0x140000 (Join-Path $dest "app.bin")

Write-Host "[4/4] model @ $modelOffset ($([Convert]::ToInt64($modelSize, 16) / 1MB) MB, ~30-40s) ..." -ForegroundColor Yellow
& $esptool --chip esp32s3 --port $Port --baud 921600 `
  --before default-reset --after no-reset `
  read-flash $modelOffset $modelSize (Join-Path $dest "model.bin")

Write-Host ""
Write-Host "✓ 备份完成: $dest" -ForegroundColor Green
Get-ChildItem $dest | Format-Table Name, Length
