[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputDirectory,
    [string]$OutputFile,
    [string]$JsonOutputFile
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$inputFull = [System.IO.Path]::GetFullPath($InputDirectory)
if (-not (Test-Path -LiteralPath $inputFull -PathType Container)) {
    throw "Checksum input directory does not exist: $inputFull"
}
if ([string]::IsNullOrWhiteSpace($OutputFile)) {
    $OutputFile = Join-Path $inputFull 'SHA256SUMS.txt'
}
$outputFull = [System.IO.Path]::GetFullPath($OutputFile)
$excludedPaths = @($outputFull)
if (-not [string]::IsNullOrWhiteSpace($JsonOutputFile)) {
    $excludedPaths += [System.IO.Path]::GetFullPath($JsonOutputFile)
}

$files = Get-ChildItem -LiteralPath $inputFull -File |
    Where-Object { $excludedPaths -notcontains $_.FullName } |
    Sort-Object -Property Name
if (-not $files) {
    throw "No package files found in $inputFull"
}

$entries = foreach ($file in $files) {
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    [pscustomobject]@{
        name = $file.Name
        size = $file.Length
        sha256 = $hash
    }
}

$text = (($entries | ForEach-Object { "$($_.sha256) *$($_.name)" }) -join "`n") + "`n"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($outputFull, $text, $utf8NoBom)

if (-not [string]::IsNullOrWhiteSpace($JsonOutputFile)) {
    $jsonFull = [System.IO.Path]::GetFullPath($JsonOutputFile)
    $json = [ordered]@{
        schema = 1
        generatedBy = 'packaging/generate-checksums.ps1'
        files = @($entries)
    } | ConvertTo-Json -Depth 5
    [System.IO.File]::WriteAllText($jsonFull, $json + "`n", $utf8NoBom)
}

Get-Content -LiteralPath $outputFull
