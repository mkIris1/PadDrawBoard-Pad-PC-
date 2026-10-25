[CmdletBinding()]
param(
    [string]$SourceDirectory = (Join-Path $PSScriptRoot '..'),
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '.work/windows-build'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'dist/windows'),
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$Generator = 'Visual Studio 17 2022',
    [string]$PlatformToolsArchivePath,
    [switch]$SkipNsis
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Native([string]$File, [string[]]$Arguments) {
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$File failed with exit code $LASTEXITCODE"
    }
}

$sourceFull = [System.IO.Path]::GetFullPath($SourceDirectory)
$buildFull = [System.IO.Path]::GetFullPath($BuildDirectory)
$outputFull = [System.IO.Path]::GetFullPath($OutputDirectory)
$adbDirectory = Join-Path $buildFull 'platform-tools-36.0.2'

$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmakeCommand) {
    throw 'cmake is required on PATH for Windows packaging'
}
$cmake = $cmakeCommand.Source
$cpack = Join-Path ([System.IO.Path]::GetDirectoryName($cmake)) 'cpack.exe'
if (-not (Test-Path -LiteralPath $cpack -PathType Leaf)) {
    throw "cpack.exe was not found next to cmake: $cpack"
}

if (-not $SkipNsis -and -not (Get-Command makensis -ErrorAction SilentlyContinue)) {
    throw 'NSIS is required for the installer. Install makensis.exe or pass -SkipNsis for ZIP-only local testing.'
}

New-Item -ItemType Directory -Force -Path $buildFull, $outputFull | Out-Null
& (Join-Path $PSScriptRoot 'acquire-platform-tools.ps1') `
    -Destination $adbDirectory -ArchivePath $PlatformToolsArchivePath -Force | Out-Host

$configure = @(
    '-S', $sourceFull,
    '-B', $buildFull,
    '-G', $Generator,
    '-DPDB_BUILD_TESTS=OFF',
    '-DPDB_PACKAGE_BUNDLED_ADB=ON',
    "-DPDB_BUNDLED_ADB_DIR=$adbDirectory"
)
if ($Generator -match 'Visual Studio') {
    $configure += @('-A', 'x64')
} else {
    $configure += "-DCMAKE_BUILD_TYPE=$Configuration"
}
Invoke-Native $cmake $configure
Invoke-Native $cmake @('--build', $buildFull, '--config', $Configuration, '--parallel')

Get-ChildItem -LiteralPath $outputFull -File -ErrorAction SilentlyContinue |
    Remove-Item -Force
$cpackConfig = Join-Path $buildFull 'CPackConfig.cmake'
if (-not (Test-Path -LiteralPath $cpackConfig -PathType Leaf)) {
    throw "CPack configuration was not generated: $cpackConfig"
}

Invoke-Native $cpack @('--config', $cpackConfig, '-G', 'ZIP', '-C', $Configuration, '-B', $outputFull)
if (-not $SkipNsis) {
    Invoke-Native $cpack @('--config', $cpackConfig, '-G', 'NSIS', '-C', $Configuration, '-B', $outputFull)
}

$checksumScript = Join-Path $PSScriptRoot 'generate-checksums.ps1'
& $checksumScript -InputDirectory $outputFull `
    -OutputFile (Join-Path $outputFull 'SHA256SUMS.txt') `
    -JsonOutputFile (Join-Path $outputFull 'SHA256SUMS.json') | Out-Host

Write-Host "Windows packages written to $outputFull"
