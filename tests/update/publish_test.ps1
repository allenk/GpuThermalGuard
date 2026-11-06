# The publish tool against GTG's own reader: what tools/update/publish-update.ps1
# writes must be what src/update accepts, signed and unsigned, and a source
# changed after publishing must be refused. The payload is a pair of test
# binaries standing in for GpuThermalGuard.exe and gtg_overlay.dll; the
# certificates are made in memory and never put in any store.
param(
    [Parameter(Mandatory = $true)][string]$Tests,
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Dll
)
$ErrorActionPreference = 'Stop'
$publish = Join-Path $PSScriptRoot '..\..\tools\update\publish-update.ps1'

function New-Signer([string]$subject) {
    $rsa = [Security.Cryptography.RSA]::Create(3072)
    $request = New-Object Security.Cryptography.X509Certificates.CertificateRequest($subject, $rsa,
        [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $usage = New-Object Security.Cryptography.OidCollection
    [void]$usage.Add((New-Object Security.Cryptography.Oid '1.3.6.1.5.5.7.3.3'))
    $request.CertificateExtensions.Add(
        (New-Object Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension($usage, $false)))
    $made = $request.CreateSelfSigned([DateTimeOffset]::UtcNow.AddDays(-1), [DateTimeOffset]::UtcNow.AddDays(1))
    $password = [Guid]::NewGuid().ToString()
    $flags = [Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet
    return [Security.Cryptography.X509Certificates.X509Certificate2]::new($made.Export('Pfx', $password), $password, $flags)
}

function Get-Pin($certificate) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($certificate.RawData)).Replace('-', '') }
    finally { $sha.Dispose() }
}

$work = Join-Path ([IO.Path]::GetTempPath()) "gtg-update-publish-$PID"
$build = Join-Path $work 'build'
$source = Join-Path $work 'source'
New-Item -ItemType Directory -Path $build, $source -Force | Out-Null
$failed = 0
function Expect([string]$what, [bool]$ok, [string]$detail) {
    if (-not $ok) { $script:failed++ }
    '{0} {1}  {2}' -f $(if ($ok) { 'ok  ' } else { 'FAIL' }), $what, $detail
}
try {
    [IO.File]::WriteAllBytes((Join-Path $build 'GpuThermalGuard.exe'), [IO.File]::ReadAllBytes($Exe))
    [IO.File]::WriteAllBytes((Join-Path $build 'gtg_overlay.dll'), [IO.File]::ReadAllBytes($Dll))

    $plain = & $publish -Source $source -Build $build -Version '1.2.3' -Unsigned -AllowLocal -Notes 'first'
    $answer = (& $Tests check-source $source overlay | Out-String).Trim()
    Expect 'unsigned release accepted' ($LASTEXITCODE -eq 0 -and $answer -match '^ok version=1\.2\.3 .*files=2 unsigned') $answer
    $answer = (& $Tests check-source $source no-overlay | Out-String).Trim()
    Expect 'no-overlay is the executable alone' ($LASTEXITCODE -eq 0 -and $answer -match '^ok version=1\.2\.3 .*files=1 unsigned') $answer

    # A GitHub release's assets: flat, one manifest, both variants on one executable.
    $assets = Join-Path $work 'assets'
    $flat = & $publish -ReleaseAssets $assets -Build $build -Version '1.2.3' -Unsigned
    $names = @(Get-ChildItem -LiteralPath $assets -File | ForEach-Object Name | Sort-Object)
    Expect 'release assets are three files' ($names.Count -eq 3 -and -not (Compare-Object $names @('GpuThermalGuard.exe', 'gtg-update.json', 'gtg_overlay.dll'))) ($names -join ',')
    $read = [IO.File]::ReadAllText((Join-Path $assets 'gtg-update.json')) | ConvertFrom-Json
    Expect 'no folders, one build' ($read.variants.overlay.dir -eq '' -and $read.variants.'no-overlay'.dir -eq '' -and
        $read.variants.overlay.build -eq $read.variants.'no-overlay'.build) "$($read.variants.overlay.dir)|$($read.variants.'no-overlay'.dir)"
    $answer = (& $Tests check-source $assets overlay | Out-String).Trim()
    Expect 'release assets read as overlay' ($LASTEXITCODE -eq 0 -and $answer -match 'version=1\.2\.3 .*files=2') $answer
    $answer = (& $Tests check-source $assets no-overlay | Out-String).Trim()
    Expect 'release assets read as no-overlay' ($LASTEXITCODE -eq 0 -and $answer -match 'version=1\.2\.3 .*files=1') $answer
    Expect 'the same release as the source' ($flat.Release -eq $plain.Release) "$($flat.Release) $($plain.Release)"

    $a = New-Signer 'CN=GTG Publish Test A'
    $b = New-Signer 'CN=GTG Publish Test B'
    $signed = & $publish -Source $source -Build $build -Version '1.2.4' -Certificate $a -AllowLocal
    $answer = (& $Tests check-source $source overlay (Get-Pin $a) | Out-String).Trim()
    Expect 'signed release accepted under its pin' ($LASTEXITCODE -eq 0 -and $answer -match 'version=1\.2\.4 .*pinned') $answer
    $answer = (& $Tests check-source $source overlay (Get-Pin $b) | Out-String).Trim()
    Expect 'refused under another pin' ($LASTEXITCODE -ne 0 -and $answer -match 'other-signer') $answer
    Expect 'each release has its own folder' ($signed.Release -ne $plain.Release -and
        (Test-Path (Join-Path $source "builds\$($plain.Release)")) -and
        (Test-Path (Join-Path $source "builds\$($signed.Release)"))) "$($plain.Release) $($signed.Release)"

    $manifest = [IO.File]::ReadAllText((Join-Path $source 'gtg-update.json')) | ConvertFrom-Json
    $published = Join-Path $source (($manifest.variants.overlay.dir -replace '/', '\') + '\gtg_overlay.dll')
    $bytes = [IO.File]::ReadAllBytes($published)
    $bytes[100] = $bytes[100] -bxor 1
    [IO.File]::WriteAllBytes($published, $bytes)
    $answer = (& $Tests check-source $source overlay | Out-String).Trim()
    Expect 'a file changed after publishing is refused' ($LASTEXITCODE -ne 0 -and $answer -match 'SHA-256') $answer

    # The release names the bytes, not the version: two more builds that differ.
    # Not $exe: variable names ignore case, and $Exe is the [string] parameter.
    $original = [IO.File]::ReadAllBytes($Exe)
    [IO.File]::WriteAllBytes((Join-Path $build 'GpuThermalGuard.exe'), [byte[]]($original + [byte]5))
    & $publish -Source $source -Build $build -Version '1.2.5' -Unsigned -AllowLocal | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $build 'GpuThermalGuard.exe'), [byte[]]($original + [byte]6))
    & $publish -Source $source -Build $build -Version '1.2.6' -Unsigned -AllowLocal -Notes 'again' | Out-Null
    $kept = @(Get-ChildItem -LiteralPath (Join-Path $source 'builds') -Directory).Count
    Expect 'the newest releases are kept, the rest removed' ($kept -le 3) "$kept folders"
    $answer = (& $Tests check-source $source overlay | Out-String).Trim()
    Expect 'the latest release is the one read' ($answer -match 'version=1\.2\.6') $answer
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
"{0} failed" -f $failed
if ($failed) { exit 1 }
