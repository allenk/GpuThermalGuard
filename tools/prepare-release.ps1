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

$manifestVersion = (Get-Content -LiteralPath 'vcpkg.json' -Raw | ConvertFrom-Json).version
if ($manifestVersion -ne $baseVersion) {
    throw "Tag $Tag does not match version $manifestVersion in vcpkg.json."
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
$overlayDll = Join-Path $BuildDirectory 'gtg_overlay.dll'
foreach ($file in @($mainExe, $overlayDll)) {
    if (-not (Test-Path -LiteralPath $file)) { throw "Missing release binary: $file" }
}

if (Test-Path -LiteralPath $OutputDirectory) {
    Remove-Item -LiteralPath $OutputDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

# Two packages around one EXE. The overlay is the DLL beside it: without the
# DLL the EXE shows no overlay controls and registers no hotkey, so the same
# binary is right for both. The read-only probe is a development tool and is
# built from source, not shipped. Signing state is stated in the release notes, not
# in the file names, so a later signed build keeps the same names.
$documents = @('LICENSE', 'THIRD_PARTY_NOTICES.md', 'README.md', 'README.zh-TW.md')
$packages = @(
    @{ Stem = "GpuThermalGuard-$version-windows-x64";            Files = @($mainExe, $overlayDll) },
    @{ Stem = "GpuThermalGuard-$version-no-overlay-windows-x64"; Files = @($mainExe) }
)
$sums = [System.Collections.Generic.List[string]]::new()
foreach ($package in $packages) {
    $packageRoot = Join-Path $OutputDirectory $package.Stem
    New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
    Copy-Item -LiteralPath ($package.Files + $documents) -Destination $packageRoot
    $zipPath = Join-Path $OutputDirectory "$($package.Stem).zip"
    Compress-Archive -Path "$packageRoot\*" -DestinationPath $zipPath -CompressionLevel Optimal
    Remove-Item -LiteralPath $packageRoot -Recurse -Force
    $hash = Get-FileHash -LiteralPath $zipPath -Algorithm SHA256
    $sums.Add("$($hash.Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($zipPath))")
}
($sums -join "`n") |
    Set-Content -LiteralPath (Join-Path $OutputDirectory 'SHA256SUMS.txt') -Encoding utf8NoBOM

$notes = [System.Collections.Generic.List[string]]::new()
$notes.Add("# GpuThermalGuard $Tag")
$notes.Add('')
$notes.AddRange([string[]]@(
    '| Download | Contents |',
    '| --- | --- |',
    "| ``GpuThermalGuard-$version-windows-x64.zip`` **(recommended)** | ``GpuThermalGuard.exe``, ``gtg_overlay.dll`` (in-game overlay) |",
    "| ``GpuThermalGuard-$version-no-overlay-windows-x64.zip`` | ``GpuThermalGuard.exe`` -- the same EXE, without the overlay |",
    ''
))
if ($Unsigned) {
    $notes.Add('> [!WARNING]')
    $notes.Add('> **Not Authenticode-signed.** The executables and the overlay DLL carry no publisher signature. Verify each ZIP with `SHA256SUMS.txt` and the GitHub Actions build-provenance attestation (`gh attestation verify <zip> -R allenk/GpuThermalGuard`) before running it. Some antivirus products flag programs that load a DLL into another process; the overlay does that, by design and only when you press its hotkey.')
    $notes.Add('')
}
$notes.AddRange([string[]]@(
    $section.Groups['body'].Value.Trim(),
    '',
    "[Full changelog](https://github.com/allenk/GpuThermalGuard/blob/$Tag/CHANGELOG.md)",
    '',
    '> The executable requests administrator privileges.'
))
($notes -join "`n") |
    Set-Content -LiteralPath (Join-Path $OutputDirectory 'release-notes.md') -Encoding utf8NoBOM

Write-Host "Prepared release packages for $Tag in $OutputDirectory."
