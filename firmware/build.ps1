param(
    [switch]$Flash,
    [string]$Port = "COM4"
)

$ErrorActionPreference = "Stop"
$idfPath = "C:\Users\Joe\esp\v5.5.6\esp-idf"
$idfProfile = "C:\Espressif\tools\Microsoft.v5.5.6.PowerShell_profile.ps1"

if (-not (Test-Path $idfPath) -or -not (Test-Path $idfProfile)) {
    throw "ESP-IDF or its activated PowerShell profile was not found"
}

. $idfProfile
$env:PYTHONUTF8 = "1"
$env:IDF_TOOLCHAIN = "clang"

# The offline tool bundle ships a target-prefixed assembler, while this
# Windows GCC driver asks PATH for the generic name "as". Keep the workaround
# local to this project instead of modifying the system/MSYS installation.
$xtensaBin = "C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin"
$shimDir = Join-Path $PSScriptRoot ".idf-shims"
New-Item -ItemType Directory -Force -Path $shimDir | Out-Null
Remove-Item (Join-Path $shimDir "as.exe") -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $shimDir "ld.exe") -Force -ErrorAction SilentlyContinue
Set-Content -Path (Join-Path $shimDir "as.bat") -Encoding ascii -Value @"
@echo off
"$xtensaBin\xtensa-esp32s3-elf-as.exe" %*
exit /b %ERRORLEVEL%
"@
Set-Content -Path (Join-Path $shimDir "ld.bat") -Encoding ascii -Value @"
@echo off
"$xtensaBin\xtensa-esp32s3-elf-ld.exe" %*
exit /b %ERRORLEVEL%
"@
$env:PATH = "$shimDir;$xtensaBin\..\lib\bfd-plugins;$env:PATH"

Set-Location $PSScriptRoot

if ((Test-Path "build") -and -not (Test-Path "build\CMakeCache.txt")) {
    Remove-Item "build" -Recurse -Force
}

idf.py set-target esp32s3

# Keep local test credentials out of source files. When Wifi2.txt exists in
# the project root, apply its hotspot values after set-target regenerates
# sdkconfig. The values are still embedded in the test firmware image.
$wifiFile = Join-Path $PSScriptRoot "..\Wifi3.txt"
if (-not (Test-Path $wifiFile)) {
    $wifiFile = Join-Path $PSScriptRoot "..\Wifi2.txt"
}
if (Test-Path $wifiFile) {
    $wifiLines = Get-Content $wifiFile
    $ssid = ($wifiLines | Where-Object { $_ -match '^SSID:' }) -replace '^SSID:', ''
    $password = ($wifiLines | Where-Object { $_ -match '^password:' }) -replace '^password:', ''
    $sdkconfig = Get-Content "sdkconfig" -Raw
    $sdkconfig = $sdkconfig -replace 'CONFIG_ICEBREAKER_WIFI_SSID="[^"]*"', ('CONFIG_ICEBREAKER_WIFI_SSID="' + $ssid + '"')
    $sdkconfig = $sdkconfig -replace 'CONFIG_ICEBREAKER_WIFI_PASSWORD="[^"]*"', ('CONFIG_ICEBREAKER_WIFI_PASSWORD="' + $password + '"')
    $sdkconfig = $sdkconfig -replace 'CONFIG_ICEBREAKER_GATEWAY_URI="[^"]*"', 'CONFIG_ICEBREAKER_GATEWAY_URI="ws://192.168.137.1:8765"'
    Set-Content "sdkconfig" -Value $sdkconfig -NoNewline
    idf.py reconfigure
}
idf.py build

if ($Flash) {
    idf.py -p $Port flash monitor
}
