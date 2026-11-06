# The signer pin end to end: copies of a test binary signed with certificates
# made in memory -- never put in any store -- and checked by the updater's own
# VerifySigner through gtg_update_tests verify-signer.
param([Parameter(Mandatory = $true)][string]$Tests)
$ErrorActionPreference = 'Stop'

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

function Set-Signature([string]$file, $certificate) {
    $signed = Set-AuthenticodeSignature -FilePath $file -Certificate $certificate -HashAlgorithm SHA256
    # UnknownError: signed by a root this machine does not trust, which is the point.
    if ($signed.Status -ne 'Valid' -and $signed.Status -ne 'UnknownError') {
        throw "signing $file failed: $($signed.Status) $($signed.StatusMessage)"
    }
}

$work = Join-Path ([IO.Path]::GetTempPath()) "gtg-update-trust-$PID"
New-Item -ItemType Directory -Path $work -Force | Out-Null
try {
    $a = New-Signer 'CN=GTG Update Test Signer A'
    $b = New-Signer 'CN=GTG Update Test Signer B'
    $pin = Get-Pin $a
    $bytes = [IO.File]::ReadAllBytes($Tests)
    foreach ($name in 'unsigned.exe', 'a.exe', 'b.exe', 'changed.exe') {
        [IO.File]::WriteAllBytes((Join-Path $work $name), $bytes)
    }
    Set-Signature (Join-Path $work 'a.exe') $a
    Set-Signature (Join-Path $work 'b.exe') $b
    $changed = Join-Path $work 'changed.exe'
    Set-Signature $changed $a
    $signed = [IO.File]::ReadAllBytes($changed)
    $signed[0x1000] = $signed[0x1000] -bxor 0x01
    [IO.File]::WriteAllBytes($changed, $signed)

    $expected = [ordered]@{
        'unsigned.exe' = 'unsigned'
        'a.exe'        = 'pinned'
        'b.exe'        = 'other-signer'
        'changed.exe'  = 'invalid'
    }
    $failed = 0
    foreach ($name in $expected.Keys) {
        $answer = (& $Tests verify-signer (Join-Path $work $name) $pin | Out-String).Trim()
        $verdict = ($answer -split ' ')[0]
        $ok = $verdict -eq $expected[$name]
        if (-not $ok) { $failed++ }
        '{0} {1,-12} expected {2,-12} got {3}' -f $(if ($ok) { 'ok  ' } else { 'FAIL' }), $name, $expected[$name], $answer
    }
    # The pin itself is the guard: the right signature with another pin is someone else's.
    $other = (& $Tests verify-signer (Join-Path $work 'a.exe') (Get-Pin $b) | Out-String).Trim()
    if (($other -split ' ')[0] -ne 'other-signer') { $failed++; "FAIL a.exe under B's pin: $other" }
    "{0} failed" -f $failed
    if ($failed) { exit 1 }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
