[CmdletBinding()]
param(
  [string]$AdbPath,
  [string]$Serial
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($AdbPath)) {
  $scriptRoot = Split-Path -Parent $PSScriptRoot
  $bundled = @(
    (Join-Path $scriptRoot 'adb\36.0.2\adb.exe'),
    (Join-Path $scriptRoot 'adb\platform-tools\adb.exe')
  )
  $AdbPath = $bundled | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if ([string]::IsNullOrWhiteSpace($AdbPath)) {
  $AdbPath = (Get-Command adb -ErrorAction SilentlyContinue).Source
}
if ([string]::IsNullOrWhiteSpace($AdbPath)) {
  throw 'adb 36.0.2 was not found. Pass -AdbPath explicitly.'
}

function Invoke-AdbSafe([string[]]$Arguments) {
  & $AdbPath @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "adb exited with code $LASTEXITCODE"
  }
}

Write-Output '=== adb version ==='
Invoke-AdbSafe @('version')
Write-Output '=== devices ==='
$deviceArguments = @('devices', '-l')
if (-not [string]::IsNullOrWhiteSpace($Serial)) {
  $deviceArguments = @('-s', $Serial) + $deviceArguments
}
Invoke-AdbSafe $deviceArguments

if (-not [string]::IsNullOrWhiteSpace($Serial)) {
  Write-Output '=== reverse status ==='
  Invoke-AdbSafe @('-s', $Serial, 'reverse', '--list')
  Write-Output '=== loopback RTT probe ==='
  $start = [System.Diagnostics.Stopwatch]::GetTimestamp()
  Invoke-AdbSafe @('-s', $Serial, 'shell', 'echo', 'pdb_rtt_probe')
  $elapsed = [System.Diagnostics.Stopwatch]::GetTimestamp() - $start
  $milliseconds = 1000.0 * $elapsed / [System.Diagnostics.Stopwatch]::Frequency
  Write-Output ('RTT_ms={0:N3}' -f $milliseconds)
}
