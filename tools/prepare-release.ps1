[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^v\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$')]
    [string] $Tag,
    [string] $BuildDirectory = 'out/build/release-ci/Release',
    [string] $OutputDirectory = 'dist',
    [switch] $Unsigned,
    [switch] $ValidateOnly
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root

$version = $Tag.Substring(1)
$baseVersion = ($version -split '-', 2)[0]
$parts = $baseVersion.Split('.')
$fourPartVersion = "$($parts[0]).$($parts[1]).$($parts[2]).0"

$cmake = Get-Content -LiteralPath 'CMakeLists.txt' -Raw
if ($cmake -notmatch 'project\(GpuThermalGuard VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
    throw 'Unable to read the project version from CMakeLists.txt.'
}
if ($Matches[1] -ne $baseVersion) {
    throw "Tag $Tag does not match CMake project version $($Matches[1])."
}

$resource = Get-Content -LiteralPath 'src/resources/GpuThermalGuard.rc' -Raw
if ($resource -notmatch "(?m)^ FILEVERSION $($parts[0]),$($parts[1]),$($parts[2]),0$") {
    throw "Tag $Tag does not match the Windows FILEVERSION."
}
if ($resource -notmatch [regex]::Escape("VALUE `"ProductVersion`", `"$fourPartVersion\0`"")) {
    throw "Tag $Tag does not match the Windows ProductVersion string."
}

$manifest = Get-Content -LiteralPath 'src/resources/GpuThermalGuard.manifest' -Raw
if ($manifest -notmatch [regex]::Escape("version=`"$fourPartVersion`"")) {
    throw "Tag $Tag does not match the application manifest version."
}

$changelog = Get-Content -LiteralPath 'CHANGELOG.md' -Raw
$escapedVersion = [regex]::Escape($version)
$section = [regex]::Match(
    $changelog,
    "(?ms)^## \[$escapedVersion\][^\r\n]*\r?\n(?<body>.*?)(?=^## \[|^\[Unreleased\]:|\z)"
)
if (-not $section.Success) {
    throw "CHANGELOG.md has no section for $version."
}

if ($ValidateOnly) {
    Write-Host "Release metadata is consistent for $Tag."
    return
}

$mainExe = Join-Path $BuildDirectory 'GpuThermalGuard.exe'
$probeExe = Join-Path $BuildDirectory 'GpuThermalGuardProbe.exe'
foreach ($file in @($mainExe, $probeExe)) {
    if (-not (Test-Path -LiteralPath $file)) { throw "Missing release binary: $file" }
}

if (Test-Path -LiteralPath $OutputDirectory) {
    Remove-Item -LiteralPath $OutputDirectory -Recurse -Force
}
$packageStem = "GpuThermalGuard-$version-windows-x64"
if ($Unsigned) {
    $packageStem = "GpuThermalGuard-$version-unsigned-windows-x64"
}
$packageRoot = Join-Path $OutputDirectory $packageStem
New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
Copy-Item -LiteralPath $mainExe, $probeExe, 'LICENSE', 'THIRD_PARTY_NOTICES.md',
    'README.md', 'README.zh-TW.md' -Destination $packageRoot

$zipPath = Join-Path $OutputDirectory "$packageStem.zip"
Compress-Archive -Path "$packageRoot\*" -DestinationPath $zipPath -CompressionLevel Optimal
Remove-Item -LiteralPath $packageRoot -Recurse -Force

$hash = Get-FileHash -LiteralPath $zipPath -Algorithm SHA256
"$($hash.Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($zipPath))" |
    Set-Content -LiteralPath (Join-Path $OutputDirectory 'SHA256SUMS.txt') -Encoding utf8NoBOM

$notes = [System.Collections.Generic.List[string]]::new()
$notes.Add("# GpuThermalGuard $Tag")
$notes.Add('')
if ($Unsigned) {
    $notes.Add('> [!WARNING]')
    $notes.Add('> **UNSIGNED PRERELEASE:** The executables in this archive do not have an Authenticode publisher signature. Verify the ZIP with `SHA256SUMS.txt` and the GitHub Actions build-provenance attestation before running it.')
    $notes.Add('')
}
$notes.AddRange([string[]]@(
    $section.Groups['body'].Value.Trim(),
    '',
    "[Full changelog](https://github.com/allenk/GpuThermalGuard/blob/$Tag/CHANGELOG.md)",
    '',
    '> Download the ZIP and verify it against `SHA256SUMS.txt`. The executable requests administrator privileges.'
))
($notes -join "`n") |
    Set-Content -LiteralPath (Join-Path $OutputDirectory 'release-notes.md') -Encoding utf8NoBOM

Write-Host "Prepared release package for $Tag in $OutputDirectory."
