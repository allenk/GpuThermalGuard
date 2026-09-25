[CmdletBinding()]
param(
    [string] $RepositoryRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$allowedTopLevel = @(
    '.editorconfig', '.git', '.gitattributes', '.github', '.gitignore',
    'assets', 'CHANGELOG.md', 'CMakeLists.txt', 'CMakePresets.json',
    'CONTRIBUTING.md', 'Directory.Build.props', 'docs', 'LICENSE', 'README.md',
    'README.zh-TW.md', 'SECURITY.md', 'src', 'tests', 'third_party',
    'THIRD_PARTY_NOTICES.md', 'tools', 'version.cmake'
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
    'THIRD_PARTY_NOTICES.md', 'CMakeLists.txt', 'src', 'tests'
)
foreach ($path in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $RepositoryRoot $path))) {
        throw "Required public file is missing: $path"
    }
}

Write-Host 'Public surface check passed.'
