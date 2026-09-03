[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^[0-9A-Fa-f ]{40,}$')]
    [string] $CertificateThumbprint,
    [string] $Executable = (Join-Path $PSScriptRoot '..\out\build\windows-x64-release\GpuThermalGuard.exe'),
    [uri] $TimestampServer = 'http://timestamp.digicert.com'
)

$ErrorActionPreference = 'Stop'
$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$thumbprint = $CertificateThumbprint.Replace(' ', '').ToUpperInvariant()
$certificate = Get-ChildItem -Path Cert:\CurrentUser\My, Cert:\LocalMachine\My -CodeSigningCert |
    Where-Object Thumbprint -EQ $thumbprint |
    Select-Object -First 1

if ($null -eq $certificate -or -not $certificate.HasPrivateKey) {
    throw "No code-signing certificate with a private key was found: $thumbprint"
}
$signTool = Get-Command signtool.exe -ErrorAction SilentlyContinue
if ($null -eq $signTool) {
    throw 'signtool.exe was not found. Load a Visual Studio/Windows SDK developer environment.'
}

& $signTool.Source sign /sha1 $thumbprint /fd SHA256 /tr $TimestampServer.AbsoluteUri /td SHA256 $resolvedExecutable
if ($LASTEXITCODE -ne 0) { throw "signtool sign failed: $LASTEXITCODE" }
& $signTool.Source verify /pa /v $resolvedExecutable
if ($LASTEXITCODE -ne 0) { throw "signtool verify failed: $LASTEXITCODE" }

Get-AuthenticodeSignature -LiteralPath $resolvedExecutable |
    Format-List Status, StatusMessage, SignatureType, SignerCertificate, TimeStamperCertificate
