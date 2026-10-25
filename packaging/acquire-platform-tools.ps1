[CmdletBinding()]
param(
    [string]$Destination = (Join-Path $PSScriptRoot '.cache/platform-tools/36.0.2'),
    [string]$ArchivePath,
    [string]$LockFile = (Join-Path $PSScriptRoot 'platform-tools.lock.json'),
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$PinnedVersion = '36.0.2'
$PinnedUri = 'https://dl.google.com/android/repository/platform-tools_r36.0.2-win.zip'
$PinnedSha256 = 'B024D4F319D6AD3004DE1BA7B96A5C7C5F3512E8B14126308D598B4AB93DCEAD'
$PinnedAdbVersion = '36.0.2'
$ArchiveFileName = 'platform-tools_r36.0.2-windows.zip'

function Get-FullPath([string]$Path) {
    return [System.IO.Path]::GetFullPath($Path)
}

function Assert-FileHash([string]$Path, [string]$Expected) {
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($actual -ne $Expected.ToUpperInvariant()) {
        throw "SHA-256 mismatch for '$Path': expected $Expected, got $actual"
    }
}

if (-not (Test-Path -LiteralPath $LockFile -PathType Leaf)) {
    throw "Platform Tools lock file not found: $LockFile"
}
$lock = Get-Content -LiteralPath $LockFile -Raw | ConvertFrom-Json
if ([string]$lock.version -ne $PinnedVersion -or
    [string]$lock.url -ne $PinnedUri -or
    [string]$lock.sha256 -ne $PinnedSha256 -or
    [string]$lock.adbVersion -ne $PinnedAdbVersion -or
    [string]$lock.archiveFileName -ne $ArchiveFileName) {
    throw 'The Platform Tools lock file does not match the hard-coded release pin'
}

$destinationFull = Get-FullPath $Destination
$destinationRoot = [System.IO.Path]::GetPathRoot($destinationFull)
if ($destinationFull.TrimEnd('\', '/') -eq $destinationRoot.TrimEnd('\', '/')) {
    throw "Refusing to use a filesystem root as extraction destination: $destinationFull"
}

$archiveOwnedByScript = [string]::IsNullOrWhiteSpace($ArchivePath)
if ($archiveOwnedByScript) {
    $downloadRoot = Join-Path ([System.IO.Path]::GetTempPath()) 'PadDrawBoard/platform-tools'
    New-Item -ItemType Directory -Force -Path $downloadRoot | Out-Null
    $ArchivePath = Join-Path $downloadRoot $ArchiveFileName
}
$archiveFull = Get-FullPath $ArchivePath
$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    'pdb-platform-tools-' + [Guid]::NewGuid().ToString('N'))
$extractionRoot = Join-Path $temporaryRoot 'extracted'

try {
    New-Item -ItemType Directory -Force -Path $temporaryRoot | Out-Null
    New-Item -ItemType Directory -Force -Path ([System.IO.Path]::GetDirectoryName($archiveFull)) | Out-Null

    if (Test-Path -LiteralPath $archiveFull -PathType Leaf) {
        Assert-FileHash $archiveFull $PinnedSha256
    } else {
        Write-Host "Downloading fixed Platform Tools $PinnedVersion from Google..."
        Invoke-WebRequest -Uri $PinnedUri -OutFile $archiveFull -UseBasicParsing
        Assert-FileHash $archiveFull $PinnedSha256
    }

    New-Item -ItemType Directory -Force -Path $extractionRoot | Out-Null
    Expand-Archive -LiteralPath $archiveFull -DestinationPath $extractionRoot -Force
    $source = Join-Path $extractionRoot 'platform-tools'
    foreach ($required in @('adb.exe', 'NOTICE.txt', 'source.properties')) {
        if (-not (Test-Path -LiteralPath (Join-Path $source $required) -PathType Leaf)) {
            throw "The verified archive is missing platform-tools/$required"
        }
    }

    $adb = Join-Path $source 'adb.exe'
    $versionOutput = (& $adb version 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or
        $versionOutput -notmatch '(?m)^Version\s+36\.0\.2(?:[-\s]|$)') {
        throw "adb version verification failed. Output: $versionOutput"
    }

    if (Test-Path -LiteralPath $destinationFull) {
        if (-not $Force) {
            throw "Destination already exists; pass -Force only for this exact extraction directory: $destinationFull"
        }
        Remove-Item -LiteralPath $destinationFull -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $destinationFull | Out-Null
    Get-ChildItem -LiteralPath $source -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName `
            -Destination (Join-Path $destinationFull $_.Name) -Recurse -Force
    }

    Assert-FileHash $archiveFull $PinnedSha256
    $installedVersion = (& (Join-Path $destinationFull 'adb.exe') version 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or
        $installedVersion -notmatch '(?m)^Version\s+36\.0\.2(?:[-\s]|$)') {
        throw "Extracted adb failed the final version check. Output: $installedVersion"
    }

    [ordered]@{
        version = $PinnedVersion
        archive = $archiveFull
        sha256 = $PinnedSha256
        adbVersion = $PinnedAdbVersion
        destination = $destinationFull
        notice = (Join-Path $destinationFull 'NOTICE.txt')
    } | ConvertTo-Json -Depth 4
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
    if ($archiveOwnedByScript -and (Test-Path -LiteralPath $archiveFull)) {
        Remove-Item -LiteralPath $archiveFull -Force -ErrorAction SilentlyContinue
    }
}
