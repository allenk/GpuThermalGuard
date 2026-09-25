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

# The version is declared once, in version.cmake, and CMake writes it from
# there into the resource header and the application manifest. So there is one
# number to compare the tag against, where there used to be four hand-synced
# declarations that could drift apart.
#
# What replaces the other three checks is not nothing. Each template is
# checked for the placeholder it is supposed to carry: if someone ever pastes
# a literal version back into one of them, the generated file would stop
# following version.cmake and the shipped EXE would quietly disagree with its
# own tag -- exactly the failure this script exists to catch, and the one a
# single version check would no longer see.
$versionFile = Get-Content -LiteralPath 'version.cmake' -Raw
if ($versionFile -notmatch '(?m)^set\(GTG_VERSION ([0-9]+\.[0-9]+\.[0-9]+)\)') {
    throw 'Unable to read GTG_VERSION from version.cmake.'
}
if ($Matches[1] -ne $baseVersion) {
    throw "Tag $Tag does not match GTG_VERSION $($Matches[1]) in version.cmake."
}

$cmake = Get-Content -LiteralPath 'CMakeLists.txt' -Raw
if ($cmake -notmatch [regex]::Escape('project(GpuThermalGuard VERSION ${GTG_VERSION}')) {
    throw 'CMakeLists.txt no longer takes its project version from version.cmake.'
}

$versionHeader = Get-Content -LiteralPath 'src/resources/gtg_version.h.in' -Raw
foreach ($token in @('@PROJECT_VERSION_MAJOR@', '@PROJECT_VERSION_MINOR@',
                     '@PROJECT_VERSION_PATCH@', '@PROJECT_VERSION@')) {
    if ($versionHeader -notmatch [regex]::Escape($token)) {
        throw "src/resources/gtg_version.h.in no longer derives $token from the project version."
    }
}

$resource = Get-Content -LiteralPath 'src/resources/GpuThermalGuard.rc' -Raw
foreach ($token in @('FILEVERSION GTG_VERSION_COMMA', 'PRODUCTVERSION GTG_VERSION_COMMA',
                     '"FileVersion", GTG_VERSION_STR', '"ProductVersion", GTG_VERSION_STR')) {
    if ($resource -notmatch [regex]::Escape($token)) {
        throw "src/resources/GpuThermalGuard.rc no longer uses the generated $token."
    }
}

$manifest = Get-Content -LiteralPath 'src/resources/GpuThermalGuard.manifest.in' -Raw
if ($manifest -notmatch [regex]::Escape('version="@PROJECT_VERSION@.0"')) {
    throw 'src/resources/GpuThermalGuard.manifest.in no longer derives its version from the project.'
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
