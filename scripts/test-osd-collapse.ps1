param([switch]$ClickThrough)
$ErrorActionPreference = 'Stop'
# Run from a Visual Studio x64 Developer PowerShell. No GPU access is required.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'Run this script from a Visual Studio x64 Developer PowerShell.'
}
$repo = Split-Path -Parent $PSScriptRoot
$mode = if ($ClickThrough) { 1 } else { 0 }
$testDir = Join-Path $repo "out/osd-collapse-smoke-$mode"
New-Item -ItemType Directory -Path $testDir -Force | Out-Null
Push-Location $testDir
try {
    & cl.exe /nologo /std:c++20 /MT /EHsc /utf-8 /W4 /DUNICODE /D_UNICODE /DNOMINMAX /D_WIN32_WINNT=0x0A00 "/DGTG_OSD_CLICK_THROUGH=$mode" "/I$repo/src" "/I$repo/third_party/wtl/Include" "$repo/tests/osd_overlay_smoke.cpp" "$repo/src/telemetry/telemetry_history.cpp" "$repo/src/logging/logger.cpp" /Fe:osd_overlay_smoke.exe /link user32.lib gdi32.lib gdiplus.lib shell32.lib comctl32.lib wtsapi32.lib dwmapi.lib advapi32.lib
    if ($LASTEXITCODE -ne 0) { throw 'OSD smoke build failed' }
    & ./osd_overlay_smoke.exe $testDir
    if ($LASTEXITCODE -ne 0) { throw 'OSD smoke test failed' }
} finally { Pop-Location }
