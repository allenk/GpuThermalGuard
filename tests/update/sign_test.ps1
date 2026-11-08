# The release signing script against certificates made in memory -- never put
# in any store: it signs with a PFX whose certificate is the one given, refuses
# any other, refuses missing secrets, and leaves no secret in the environment.
# What it signs is checked by the updater's own VerifySigner.
param(
    [Parameter(Mandatory = $true)][string]$Script,
    [Parameter(Mandatory = $true)][string]$Tests,
    # PowerShell 7, which the release workflow runs the script in; skipped when empty.
    [string]$Pwsh = ''
)
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
    return @{ Pfx = $made.Export('Pfx', $password); Password = $password; Cer = $made.RawData }
}

function Get-Pin([byte[]]$bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-', '') }
    finally { $sha.Dispose() }
}

function Set-Secrets($signer, [bool]$withPassword = $true) {
    $env:WINDOWS_CERTIFICATE_BASE64 = [Convert]::ToBase64String($signer.Pfx)
    $env:WINDOWS_CERTIFICATE_PASSWORD = if ($withPassword) { $signer.Password } else { '' }
}

function Clear-Secrets {
    Remove-Item Env:WINDOWS_CERTIFICATE_BASE64 -ErrorAction SilentlyContinue
    Remove-Item Env:WINDOWS_CERTIFICATE_PASSWORD -ErrorAction SilentlyContinue
}

# Fresh unsigned copies of a real PE file for each case.
function New-Targets([string]$case) {
    $folder = Join-Path $work $case
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
    $exe = Join-Path $folder 'GpuThermalGuard.exe'
    $dll = Join-Path $folder 'gtg_overlay.dll'
    [IO.File]::WriteAllBytes($exe, $bytes)
    [IO.File]::WriteAllBytes($dll, $bytes)
    return @($exe, $dll)
}

function Invoke-Sign([string[]]$files) {
    try {
        $output = & $Script -Files $files -Certificate $cer -VerifyWith $Tests
        return @{ Ok = $true; Text = ($output | Out-String).Trim() }
    } catch {
        return @{ Ok = $false; Text = $_.Exception.Message }
    }
}

function Get-Verdict([string]$file, [string]$pin) {
    return ((& $Tests verify-signer $file $pin | Out-String).Trim() -split ' ')[0]
}

$failed = 0
function Check([bool]$ok, [string]$what, [string]$detail) {
    if ($ok) { "ok   $what" } else { $script:failed++; "FAIL $what -- $detail" }
}

$work = Join-Path ([IO.Path]::GetTempPath()) "gtg-update-sign-$PID"
New-Item -ItemType Directory -Path $work -Force | Out-Null
try {
    $bytes = [IO.File]::ReadAllBytes($Tests)
    $a = New-Signer 'CN=GTG Release Sign Test A'
    $b = New-Signer 'CN=GTG Release Sign Test B'
    $cer = Join-Path $work 'release-signer.cer'
    [IO.File]::WriteAllBytes($cer, $a.Cer)
    $pin = Get-Pin $a.Cer

    # The certificate given: both files signed, and pinned by GTG's own check.
    $files = New-Targets 'matching'
    Set-Secrets $a
    $result = Invoke-Sign $files
    Check $result.Ok 'the matching PFX signs' $result.Text
    foreach ($file in $files) {
        $verdict = Get-Verdict $file $pin
        Check ($verdict -eq 'pinned') "$([IO.Path]::GetFileName($file)) is pinned" $verdict
    }
    Check ([string]::IsNullOrEmpty($env:WINDOWS_CERTIFICATE_BASE64) -and
        [string]::IsNullOrEmpty($env:WINDOWS_CERTIFICATE_PASSWORD)) 'no secret left in the environment' 'still set'

    # Another certificate's PFX: refused before anything is signed.
    $files = New-Targets 'other'
    Set-Secrets $b
    $result = Invoke-Sign $files
    Check (-not $result.Ok -and $result.Text -like '*is not*') 'a PFX of another certificate is refused' $result.Text
    Check ((Get-Verdict $files[0] $pin) -eq 'unsigned') 'and nothing was signed' (Get-Verdict $files[0] $pin)
    Clear-Secrets

    # Secrets missing or wrong.
    $files = New-Targets 'missing'
    Set-Secrets $a $false
    $result = Invoke-Sign $files
    Check (-not $result.Ok -and $result.Text -like '*must both be set*') 'a missing password is refused' $result.Text
    Clear-Secrets
    $result = Invoke-Sign $files
    Check (-not $result.Ok -and $result.Text -like '*must both be set*') 'no secrets at all are refused' $result.Text
    $env:WINDOWS_CERTIFICATE_BASE64 = [Convert]::ToBase64String($a.Pfx)
    $env:WINDOWS_CERTIFICATE_PASSWORD = 'not the password'
    $result = Invoke-Sign $files
    Check (-not $result.Ok) 'a wrong password is refused' $result.Text
    Check ((Get-Verdict $files[0] $pin) -eq 'unsigned') 'and nothing was signed' (Get-Verdict $files[0] $pin)
    Clear-Secrets

    # PowerShell 7, as the release workflow runs it.
    if ($Pwsh) {
        $files = New-Targets 'pwsh'
        Set-Secrets $a
        $command = "& '{0}' -Files @('{1}', '{2}') -Certificate '{3}' -VerifyWith '{4}'" -f $Script, $files[0], $files[1], $cer, $Tests
        $output = (& $Pwsh -NoProfile -NonInteractive -Command $command 2>&1 | Out-String).Trim()
        Check ($LASTEXITCODE -eq 0) 'PowerShell 7 signs' $output
        Clear-Secrets
        foreach ($file in $files) {
            $verdict = Get-Verdict $file $pin
            Check ($verdict -eq 'pinned') "PowerShell 7: $([IO.Path]::GetFileName($file)) is pinned" $verdict
        }
    } else {
        'skip PowerShell 7 not found'
    }
    "{0} failed" -f $failed
    if ($failed) { exit 1 }
} finally {
    Clear-Secrets
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
