# Publishes a GTG build for the updater, in one of two forms.
#
# To a UNC update source: sign a private copy, hash the signed files, copy them
# into builds\<release>\<variant>\ on the source, and write gtg-update.json
# last. A GTG believes only the manifest's hashes, so a machine reading
# mid-publish gets the previous release or refuses; it never gets a mix. Each
# release has a folder of its own because a file someone is reading cannot be
# replaced; only the manifest is replaced, by rename.
#
# As the assets of a GitHub release: the same files and manifest, flat in one
# folder, since a release has no folders. GTG reads the manifest of the latest
# release and the files of the release it names.
#
# The no-overlay variant is the overlay build's executable without
# gtg_overlay.dll unless a separate build is given, as in the release packages.
#
# A certificate given here is the publisher's own: machines that pin its
# SHA-256 install only what it signed. Keep a key that machines pin out of any
# build service that signs something else.
#
#   publish-update.ps1 -Source \\host\share\GTG -Secrets <folder with codesign.*>
#   publish-update.ps1 -Source ... -Secrets ... -Root   # also replace the files
#                                                       # at the source root, for
#                                                       # installing by hand
#   publish-update.ps1 -ReleaseAssets <folder> -Unsigned  # a GitHub release's files
param(
    [string]$Source,
    # A folder to write a GitHub release's update files into, instead of a source.
    [string]$ReleaseAssets,
    # codesign.pfx, codesign.cer and codesign.env (password and thumbprint).
    [string]$Secrets,
    [switch]$Unsigned,
    # The overlay build: GpuThermalGuard.exe and gtg_overlay.dll.
    [string]$Build = (Join-Path $PSScriptRoot '..\..\out\build\windows-x64-release'),
    # Optional: a build without the overlay, published as the no-overlay variant.
    [string]$NoOverlayBuild,
    [string]$Version,
    [string]$Notes = '',
    [int]$Keep = 3,
    [switch]$Root,
    # For tests: a certificate in memory instead of -Secrets, and a local folder
    # as the source.
    [Security.Cryptography.X509Certificates.X509Certificate2]$Certificate,
    [switch]$AllowLocal
)
$ErrorActionPreference = 'Stop'

if (-not ('GtgPublishNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class GtgPublishNative {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool MoveFileExW(string from, string to, uint flags);
    // Replace in one rename: a reader sees the old file or the new one.
    public static int Replace(string from, string to) {
        return MoveFileExW(from, to, 1 | 8) ? 0 : Marshal.GetLastWin32Error();
    }
}
'@
}

function Get-Sha256([byte[]]$bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-', '') }
    finally { $sha.Dispose() }
}

# Written beside the target and renamed over it. A reader holding the target
# makes the rename fail with error 5 whatever sharing it asked for, so retry.
function Write-Atomic([string]$target, [byte[]]$bytes) {
    $partial = "$target.partial"
    [IO.File]::WriteAllBytes($partial, $bytes)
    for ($attempt = 0; ; $attempt++) {
        $error32 = [GtgPublishNative]::Replace($partial, $target)
        if ($error32 -eq 0) { return }
        if ($attempt -ge 40) { throw "cannot replace $target (error $error32)" }
        Start-Sleep -Milliseconds 100
    }
}

# The password goes from the file into a SecureString; it is never printed and
# never on a command line.
function Import-Signing([string]$dir) {
    $values = @{}
    foreach ($line in [IO.File]::ReadAllLines((Join-Path $dir 'codesign.env'))) {
        $at = $line.IndexOf('=')
        if ($at -gt 0) { $values[$line.Substring(0, $at)] = $line.Substring($at + 1) }
    }
    $secure = New-Object Security.SecureString
    foreach ($char in $values['GTG_CODESIGN_PFX_PASSWORD'].ToCharArray()) { $secure.AppendChar($char) }
    $values.Remove('GTG_CODESIGN_PFX_PASSWORD')
    $flags = [Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet
    $certificate = New-Object Security.Cryptography.X509Certificates.X509Certificate2((Join-Path $dir 'codesign.pfx'), $secure, $flags)
    $public = New-Object Security.Cryptography.X509Certificates.X509Certificate2((Join-Path $dir 'codesign.cer'))
    if ($certificate.Thumbprint -ne $values['GTG_CODESIGN_THUMBPRINT'] -or $public.Thumbprint -ne $certificate.Thumbprint) {
        throw 'codesign.pfx, codesign.cer and codesign.env do not name the same certificate'
    }
    return @{ Certificate = $certificate; Pin = (Get-Sha256 $public.RawData) }
}

if ([string]::IsNullOrEmpty($Source) -eq [string]::IsNullOrEmpty($ReleaseAssets)) { throw 'pass -Source or -ReleaseAssets' }
if ($Source -and -not $AllowLocal -and -not $Source.StartsWith('\\')) { throw 'the source is a UNC path: an elevated GTG does not see mapped drives' }
if ($ReleaseAssets -and $Root) { throw '-Root is for a source' }
if ($ReleaseAssets -and $NoOverlayBuild) { throw 'a release has one executable: no separate no-overlay build' }
# A release's files are uploaded as they are found: start from nothing.
if ($ReleaseAssets -and (Test-Path -LiteralPath $ReleaseAssets) -and
    @(Get-ChildItem -LiteralPath $ReleaseAssets -Force).Count -gt 0) {
    throw "$ReleaseAssets is not empty"
}
if (-not $Unsigned -and -not $Secrets -and -not $Certificate) { throw 'pass -Secrets, or -Unsigned for machines without a signer pin' }
if (-not $Version) {
    $line = Select-String -LiteralPath (Join-Path $PSScriptRoot '..\..\version.cmake') -Pattern '^set\(GTG_VERSION (\d+\.\d+\.\d+)\)'
    if (-not $line) { throw 'no version in version.cmake; pass -Version' }
    $Version = $line.Matches[0].Groups[1].Value
}
$builds = [ordered]@{ 'overlay' = [IO.Path]::GetFullPath($Build) }
if ($NoOverlayBuild) { $builds['no-overlay'] = [IO.Path]::GetFullPath($NoOverlayBuild) }
$flat = [bool]$ReleaseAssets
$entry = 'GpuThermalGuard.exe'
$signing = if ($Unsigned) { $null }
    elseif ($Certificate) { @{ Certificate = $Certificate; Pin = (Get-Sha256 $Certificate.RawData); Borrowed = $true } }
    else { Import-Signing $Secrets }

# 1. A private copy, signed there: never sign in the build folder or on the source.
$stage = Join-Path ([IO.Path]::GetTempPath()) ('gtg-publish-' + [DateTime]::UtcNow.Ticks)
$sets = [ordered]@{}
try {
    foreach ($variant in $builds.Keys) {
        $dir = Join-Path $stage $variant
        New-Item -ItemType Directory -Path $dir | Out-Null
        $names = @($entry)
        if ($variant -eq 'overlay') { $names += 'gtg_overlay.dll' }
        foreach ($name in $names) {
            $from = Join-Path $builds[$variant] $name
            if (-not (Test-Path -LiteralPath $from)) { throw "$variant build has no $name ($from)" }
            # Bytes, never CopyFile: CopyFile carries a Zone.Identifier stream along.
            [IO.File]::WriteAllBytes((Join-Path $dir $name), [IO.File]::ReadAllBytes($from))
        }
        $sets[$variant] = @{ dir = $dir; names = $names }
    }
    if ($signing) {
        foreach ($set in $sets.Values) {
            foreach ($name in $set.names) {
                $path = Join-Path $set.dir $name
                $signed = Set-AuthenticodeSignature -FilePath $path -Certificate $signing.Certificate -HashAlgorithm SHA256
                # UnknownError: this machine does not trust the self-signed root, which the pin makes irrelevant.
                if ($signed.Status -ne 'Valid' -and $signed.Status -ne 'UnknownError') {
                    throw "signing $name failed: $($signed.Status) $($signed.StatusMessage)"
                }
                $check = Get-AuthenticodeSignature -FilePath $path
                if (($check.Status -ne 'Valid' -and $check.Status -ne 'UnknownError') -or
                    (Get-Sha256 $check.SignerCertificate.RawData) -ne $signing.Pin) {
                    throw "$name does not verify after signing: $($check.Status)"
                }
            }
        }
    }

    # Without a build of its own, no-overlay is the overlay build's signed
    # executable: the no-overlay package is that executable without the DLL.
    if (-not $sets.Contains('no-overlay')) {
        $dir = Join-Path $stage 'no-overlay'
        New-Item -ItemType Directory -Path $dir | Out-Null
        [IO.File]::WriteAllBytes((Join-Path $dir $entry), [IO.File]::ReadAllBytes((Join-Path $sets['overlay'].dir $entry)))
        $sets['no-overlay'] = @{ dir = $dir; names = @($entry) }
    }

    # 2. Hash after signing: a signature changes the file's SHA-256.
    $variants = [ordered]@{}
    foreach ($variant in $sets.Keys) {
        $files = @()
        foreach ($name in $sets[$variant].names) {
            $bytes = [IO.File]::ReadAllBytes((Join-Path $sets[$variant].dir $name))
            $files += [ordered]@{ name = $name; size = $bytes.Length; sha256 = (Get-Sha256 $bytes) }
        }
        $variants[$variant] = [ordered]@{ build = ($files | Where-Object { $_.name -eq $entry }).sha256; files = $files }
    }
    # The release names the whole publish: the SHA-256 of "variant=BUILD"
    # lines sorted by variant name, ordinal, joined with "\n". GTG computes
    # the same and refuses a manifest whose release does not match.
    $keys = [string[]]@($variants.Keys)
    [Array]::Sort($keys, [StringComparer]::Ordinal)
    $identity = ($keys | ForEach-Object { "$_=$($variants[$_].build)" }) -join "`n"
    $release = Get-Sha256 ([Text.Encoding]::UTF8.GetBytes($identity))
    foreach ($variant in $variants.Keys) {
        $variants[$variant].dir = if ($flat) { '' } else { "builds/$($release.Substring(0, 16))/$variant" }
    }
    $manifest = [ordered]@{
        format = 1
        version = $Version
        release = $release
        # Invariant: ':' in a format is the culture's time separator otherwise.
        published = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ', [Globalization.CultureInfo]::InvariantCulture)
        notes = $Notes
        entry = $entry
        variants = $variants
    }

    if ($flat) {
        # A release's assets: each file once, then the manifest.
        New-Item -ItemType Directory -Path $ReleaseAssets -Force | Out-Null
        foreach ($name in $sets['overlay'].names) {
            [IO.File]::WriteAllBytes((Join-Path $ReleaseAssets $name), [IO.File]::ReadAllBytes((Join-Path $sets['overlay'].dir $name)))
        }
        $json = $manifest | ConvertTo-Json -Depth 6
        [IO.File]::WriteAllBytes((Join-Path $ReleaseAssets 'gtg-update.json'), [Text.UTF8Encoding]::new($false).GetBytes($json))
        return [pscustomobject]@{
            Version = $Version
            Release = $release.Substring(0, 16)
            Published = $manifest.published
            Signed = [bool]$signing
            Variants = ($variants.Keys | ForEach-Object { "$_ build $($variants[$_].build.Substring(0, 8))" }) -join ', '
        }
    }

    # 3. Copy into a folder of this release's own on the source.
    foreach ($variant in $sets.Keys) {
        $target = Join-Path $Source ($variants[$variant].dir -replace '/', '\')
        New-Item -ItemType Directory -Path $target -Force | Out-Null
        foreach ($name in $sets[$variant].names) {
            Write-Atomic (Join-Path $target $name) ([IO.File]::ReadAllBytes((Join-Path $sets[$variant].dir $name)))
        }
    }

    # 4. The manifest last.
    $json = $manifest | ConvertTo-Json -Depth 6
    Write-Atomic (Join-Path $Source 'gtg-update.json') ([Text.UTF8Encoding]::new($false).GetBytes($json))

    # For installing by hand: the overlay build at the root, as before.
    if ($Root) {
        foreach ($name in $sets['overlay'].names) {
            Write-Atomic (Join-Path $Source $name) ([IO.File]::ReadAllBytes((Join-Path $sets['overlay'].dir $name)))
        }
    }

    # 5. Keep the newest releases; one a machine still has open stays until next time.
    $buildsRoot = Join-Path $Source 'builds'
    $current = $release.Substring(0, 16)
    $old = @(Get-ChildItem -LiteralPath $buildsRoot -Directory | Where-Object { $_.Name -ne $current } |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -Skip ([Math]::Max(0, $Keep - 1)))
    foreach ($dir in $old) {
        try { Remove-Item -LiteralPath $dir.FullName -Recurse -Force } catch { "kept $($dir.Name): in use" }
    }
} finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    if ($signing -and -not $signing.Borrowed) { $signing.Certificate.Dispose() }
}

[pscustomobject]@{
    Version = $Version
    Release = $release.Substring(0, 16)
    Published = $manifest.published
    Signed = [bool]$signing
    Variants = ($variants.Keys | ForEach-Object { "$_ build $($variants[$_].build.Substring(0, 8))" }) -join ', '
}
