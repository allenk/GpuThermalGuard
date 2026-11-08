# The supervisor with a Tray on a machine that has no NVIDIA GPU, against the
# update stand-in, which runs the real supervisor code. A Tray that monitors
# only is not replaced; one that stops its heartbeat, or that pinned a GPU
# first (a driver reset), is replaced as a recovery; and the fresh start a
# Tray asks for when a GPU appears is an ordinary start, without a backoff.
# Nothing here starts GTG; the stand-in has its own supervisor singleton.
param(
    [Parameter(Mandatory = $true)][string]$Standin,
    [string]$Sandbox = (Join-Path ([IO.Path]::GetTempPath()) "gtg-no-gpu-$PID")
)
$ErrorActionPreference = 'Stop'

$watchdog = 3000
$env:GTG_STANDIN_WATCHDOG_MS = "$watchdog"

function Copy-Variant([string]$source, [string]$target, [string]$name) {
    $bytes = [IO.File]::ReadAllBytes($source)
    $marker = [Text.Encoding]::ASCII.GetBytes("`nLABBUILD:$name`n")
    [IO.File]::WriteAllBytes($target, [byte[]]($bytes + $marker))
}

function Get-Under([string]$dir) {
    $prefix = $dir.TrimEnd('\') + '\'
    @(Get-Process | Where-Object { $_.Path -and $_.Path.StartsWith($prefix, 'OrdinalIgnoreCase') })
}

function Stop-Sandbox { Get-Under $Sandbox | Stop-Process -Force -ErrorAction SilentlyContinue }

function Wait-Until([scriptblock]$condition, [int]$ms) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($ms)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (& $condition) { return $true }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

function Read-Json([string]$file) {
    try { return [IO.File]::ReadAllText($file) | ConvertFrom-Json } catch { return $null }
}

# The stand-ins append to lab.log while this reads it, each line through a
# handle of its own: ReadAllText shares only reading and fails on that handle.
function Get-Log([string]$dir) {
    $file = Join-Path $dir 'lab.log'
    if (-not (Test-Path -LiteralPath $file)) { return '' }
    $share = [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
    $reader = New-Object IO.StreamReader((New-Object IO.FileStream($file, [IO.FileMode]::Open, [IO.FileAccess]::Read, $share)))
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}

# The recovery flag of every Tray start, in order.
function Get-Starts([string]$dir) {
    @([regex]::Matches((Get-Log $dir), 'TRAY started build=\S+ .*?recovery=(\d)') |
        ForEach-Object { [int]$_.Groups[1].Value })
}

function Send-Request([string]$dir, [string]$flags) {
    $file = Join-Path $dir 'lab-request.json'
    [IO.File]::WriteAllText("$file.tmp", "{`"flags`":`"$flags`"}")
    Move-Item -LiteralPath "$file.tmp" -Destination $file -Force
}

function Start-Lab([string]$name, [string]$build) {
    Stop-Sandbox
    $dir = Join-Path $Sandbox $name
    if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
    New-Item -ItemType Directory -Path $dir | Out-Null
    Copy-Variant $Standin (Join-Path $dir 'app.exe') $build
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = Join-Path $dir 'app.exe'
    $start.WorkingDirectory = $dir
    $start.UseShellExecute = $false
    [void][Diagnostics.Process]::Start($start)
    if (-not (Wait-Until { $r = Read-Json (Join-Path $dir 'lab-ready.json'); $r -and $r.build -eq $build } 10000)) {
        throw "the $build build did not start in $dir"
    }
    return $dir
}

$results = New-Object System.Collections.ArrayList
function Check([string]$scenario, [string]$what, [bool]$ok, [string]$detail) {
    [void]$results.Add([pscustomobject]@{ Scenario = $scenario; Check = $what; Ok = $ok; Detail = $detail })
}

New-Item -ItemType Directory -Path $Sandbox -Force | Out-Null
try {
    # Monitors only: outlives the watchdog three times over.
    $dir = Start-Lab 'stays' 'nogpu'
    Start-Sleep -Milliseconds ($watchdog * 3)
    $starts = Get-Starts $dir
    Check 'stays' 'not replaced' ($starts.Count -eq 1) "$($starts.Count) starts in $($watchdog * 3) ms"
    Check 'stays' 'one supervisor + one tray' ((Get-Under $dir).Count -eq 2) ''

    # A GPU appears: replaced at once, as an ordinary start.
    $clock = [Diagnostics.Stopwatch]::StartNew()
    Send-Request $dir 'fresh'
    [void](Wait-Until { (Get-Starts $dir).Count -ge 2 } 6000)
    $ms = $clock.ElapsedMilliseconds
    $starts = Get-Starts $dir
    Check 'fresh' 'replaced' ($starts.Count -eq 2) "$($starts.Count) starts"
    Check 'fresh' 'an ordinary start, not a recovery' ($starts.Count -ge 2 -and $starts[1] -eq 0) "recovery=$($starts[1])"
    Check 'fresh' 'without a backoff' ($ms -lt 1900) "$ms ms"

    # Stops its heartbeat: the watchdog replaces it, as a recovery.
    $dir = Start-Lab 'stall' 'nogpu'
    Send-Request $dir 'stall'
    [void](Wait-Until { (Get-Starts $dir).Count -ge 2 } ($watchdog + 8000))
    $starts = Get-Starts $dir
    Check 'stall' 'replaced' ($starts.Count -ge 2) "$($starts.Count) starts"
    Check 'stall' 'as a recovery' ($starts.Count -ge 2 -and $starts[1] -eq 1) "recovery=$($starts[1])"

    # Pinned a GPU, then finds none: a driver reset, replaced as a recovery.
    $dir = Start-Lab 'pinned' 'pinned-nogpu'
    [void](Wait-Until { (Get-Starts $dir).Count -ge 2 } ($watchdog + 8000))
    $starts = Get-Starts $dir
    Check 'pinned' 'replaced' ($starts.Count -ge 2) "$($starts.Count) starts"
    Check 'pinned' 'as a recovery' ($starts.Count -ge 2 -and $starts[1] -eq 1) "recovery=$($starts[1])"
} finally {
    Stop-Sandbox
    Remove-Item Env:\GTG_STANDIN_WATCHDOG_MS -ErrorAction SilentlyContinue
}

$results | Format-Table -AutoSize -Wrap | Out-String -Width 200
$failed = @($results | Where-Object { -not $_.Ok })
"{0} checks, {1} failed" -f $results.Count, $failed.Count
if ($failed.Count -eq 0) { Remove-Item -LiteralPath $Sandbox -Recurse -Force -ErrorAction SilentlyContinue }
if ($failed.Count) { exit 1 }
