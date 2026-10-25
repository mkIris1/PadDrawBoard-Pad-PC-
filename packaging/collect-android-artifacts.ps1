[CmdletBinding()]
param(
    [string]$AndroidDirectory = (Join-Path $PSScriptRoot '..\android'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\artifacts\android'),
    [string]$Version = '0.1.0',
    [ValidateSet('Signed', 'Unsigned')]
    [string]$ExpectedReleaseSigning = 'Unsigned'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$androidFull = [System.IO.Path]::GetFullPath($AndroidDirectory)
$outputFull = [System.IO.Path]::GetFullPath($OutputDirectory)
$debugApk = Join-Path $androidFull 'app/build/outputs/apk/debug/app-debug.apk'
$releaseName = if ($ExpectedReleaseSigning -eq 'Signed') { 'app-release.apk' } else { 'app-release-unsigned.apk' }
$releaseApk = Join-Path $androidFull ("app/build/outputs/apk/release/{0}" -f $releaseName)

foreach ($apk in @($debugApk, $releaseApk)) {
    if (-not (Test-Path -LiteralPath $apk -PathType Leaf)) {
        throw "Expected Android artifact was not found: $apk"
    }
}

New-Item -ItemType Directory -Force -Path $outputFull | Out-Null
Copy-Item -LiteralPath $debugApk `
    -Destination (Join-Path $outputFull "PadDrawBoard-$Version-android-debug.apk") -Force
$releaseOutputName = if ($ExpectedReleaseSigning -eq 'Signed') {
    "PadDrawBoard-$Version-android-release-signed.apk"
} else {
    "PadDrawBoard-$Version-android-release-unsigned.apk"
}
Copy-Item -LiteralPath $releaseApk -Destination (Join-Path $outputFull $releaseOutputName) -Force

$releaseStatus = if ($ExpectedReleaseSigning -eq 'Signed') {
    'signed by the externally supplied release keystore; trust still depends on the published certificate.'
} else {
    'unsigned (app-release-unsigned.apk); install or distribute only after signing with a trusted release key.'
}
$status = @"
PadDrawBoard Android artifact signing status
============================================
debug APK: Gradle debug key; suitable for development only and not a trusted public signature.
release APK: $releaseStatus
Windows ZIP and NSIS artifacts are unsigned unless an external Authenticode signing step is configured.
"@
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText((Join-Path $outputFull 'ANDROID_SIGNING_STATUS.txt'), $status, $utf8NoBom)
