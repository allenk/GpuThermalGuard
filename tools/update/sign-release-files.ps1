# Signs a public release's binaries with the project's own certificate, in the
# release workflow, before the packages and the update manifest are made: the
# signature changes each file's SHA-256.
#
# The key comes from two environment variables (the workflow's secrets): the
# PFX as base64 and its password. Both are read once and removed from this
# process's environment, so nothing started from here inherits them; the
# password never appears on a command line and the PFX is never written to
# disk. A PFX whose certificate is not -Certificate is refused: GTG pins that
# certificate, so signing with any other key would publish a release no
# updated machine accepts. Each signed file is then checked by GTG's own
# signer check (-VerifyWith gtg_update_tests).
#
#   sign-release-files.ps1 -Files <exe>, <dll> -Certificate tools/update/public-release-signer.cer `
#       -VerifyWith <gtg_update_tests.exe>
#
# Windows PowerShell 5.1 and PowerShell 7 both run it.
param(
    [Parameter(Mandatory = $true)][string[]]$Files,
    [Parameter(Mandatory = $true)][string]$Certificate,
    [Parameter(Mandatory = $true)][string]$VerifyWith
)
$ErrorActionPreference = 'Stop'

function Get-Sha256([byte[]]$bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-', '') }
    finally { $sha.Dispose() }
}

function Take-Secret([string]$name) {
    $value = [Environment]::GetEnvironmentVariable($name, 'Process')
    [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    return $value
}

$encoded = Take-Secret 'WINDOWS_CERTIFICATE_BASE64'
$password = Take-Secret 'WINDOWS_CERTIFICATE_PASSWORD'
if ([string]::IsNullOrWhiteSpace($encoded) -or [string]::IsNullOrEmpty($password)) {
    throw 'WINDOWS_CERTIFICATE_BASE64 and WINDOWS_CERTIFICATE_PASSWORD must both be set: a public release is always signed'
}
$secure = New-Object Security.SecureString
foreach ($char in $password.ToCharArray()) { $secure.AppendChar($char) }
$password = $null
$pfx = [Convert]::FromBase64String($encoded)
$encoded = $null

$expected = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Certificate).Path)
$pin = Get-Sha256 $expected
$flags = [Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet
$signer = [Security.Cryptography.X509Certificates.X509Certificate2]::new($pfx, $secure, $flags)
[Array]::Clear($pfx, 0, $pfx.Length)
try {
    if (-not $signer.HasPrivateKey) { throw 'the PFX holds no private key' }
    if ((Get-Sha256 $signer.RawData) -ne $pin) {
        throw "the PFX's certificate is not $Certificate; GTG would refuse what it signs"
    }
    foreach ($file in $Files) {
        $path = (Resolve-Path -LiteralPath $file).Path
        $signed = Set-AuthenticodeSignature -FilePath $path -Certificate $signer -HashAlgorithm SHA256
        # UnknownError: a root this machine does not trust, which the pin makes irrelevant.
        if ($signed.Status -ne 'Valid' -and $signed.Status -ne 'UnknownError') {
            throw "signing $path failed: $($signed.Status) $($signed.StatusMessage)"
        }
        $answer = (& $VerifyWith verify-signer $path $pin | Out-String).Trim()
        if (($answer -split ' ')[0] -ne 'pinned') { throw "$path does not verify after signing: $answer" }
        "signed $([IO.Path]::GetFileName($path)) $answer"
    }
} finally {
    $signer.Dispose()
    $secure.Dispose()
}
