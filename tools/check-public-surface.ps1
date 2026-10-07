[CmdletBinding()]
param(
    [string] $RepositoryRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$allowedTopLevel = @(
    '.editorconfig', '.git', '.gitattributes', '.github', '.gitignore', '.gitmodules',
    'assets', 'CHANGELOG.md', 'CMakeLists.txt', 'CMakePresets.json',
    'CONTRIBUTING.md', 'Directory.Build.props', 'docs', 'LICENSE', 'overlay', 'README.md',
    'README.zh-TW.md', 'SECURITY.md', 'src', 'tests', 'third_party',
    'THIRD_PARTY_NOTICES.md', 'tools', 'vcpkg.json', 'version.cmake'
)

$tracked = & git -C $RepositoryRoot ls-files
if ($LASTEXITCODE -ne 0) { throw 'Unable to enumerate tracked public files.' }
$trackedTopLevel = $tracked | ForEach-Object { ($_ -split '[/\\]', 2)[0] } | Sort-Object -Unique
$unexpected = $trackedTopLevel | Where-Object { $_ -notin $allowedTopLevel }
if ($unexpected) {
    $names = ($unexpected | Sort-Object) -join ', '
    throw "Unexpected top-level public surface: $names"
}

$required = @(
    'README.md', 'README.zh-TW.md', 'CHANGELOG.md', 'LICENSE',
    'THIRD_PARTY_NOTICES.md', 'CMakeLists.txt', 'src', 'tests', 'overlay', 'vcpkg.json'
)
foreach ($path in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $RepositoryRoot $path))) {
        throw "Required public file is missing: $path"
    }
}

# Contents, not just directories. The public tree is curated from a private
# one, and a directory allowlist cannot see a private name inside an allowed
# file. Vendored third-party sources are not ours to edit and are skipped;
# submodules are gitlinks and have no content here.
$forbidden = @(
    @{ Pattern = '(?i)\bjewel';                 Reason = 'a commercial game name' },
    @{ Pattern = '(?i)mad[ _-]?king';           Reason = 'a commercial game name' },
    @{ Pattern = '(?i)minecraft|dungeons';      Reason = 'a commercial game name' },
    @{ Pattern = '(?i)redemption';              Reason = 'a commercial game name' },
    @{ Pattern = '(?i)kwyport';                 Reason = 'a private remote' },
    @{ Pattern = '(?i)\b[A-Z]:[\\/]Users[\\/]'; Reason = 'a machine path' },
    @{ Pattern = '(?i)\b[A-Z]:[\\/]CodeForge'; Reason = 'a machine path' }
)
$binary = '(?i)\.(png|jpg|jpeg|gif|ico|bmp|zip|dll|exe|pdb)$'
$hits = [System.Collections.Generic.List[string]]::new()
foreach ($rel in $tracked) {
    if ($rel -like 'third_party/*' -or $rel -match $binary) { continue }
    if ($rel -eq 'tools/check-public-surface.ps1') { continue }
    $path = Join-Path $RepositoryRoot $rel
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
    $lineNumber = 0
    foreach ($line in [IO.File]::ReadLines($path)) {
        ++$lineNumber
        foreach ($rule in $forbidden) {
            if ($line -match $rule.Pattern) {
                $hits.Add("${rel}:${lineNumber}: $($rule.Reason) ($($Matches[0]))")
            }
        }
    }
}
if ($hits.Count -gt 0) {
    $hits | ForEach-Object { Write-Host $_ }
    throw "Public content check failed: $($hits.Count) line(s) name private material."
}

Write-Host 'Public surface check passed.'
