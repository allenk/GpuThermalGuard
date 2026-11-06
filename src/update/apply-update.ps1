# GpuThermalGuard update: installs the files a plan names once the running
# build has exited, starts the new build and waits for it to report ready;
# puts the old build back when it does not.
#
# GTG compiles this script into its executable and writes it out next to the
# plan for each update, so what runs is the copy the trusted, running build
# carries. It reads only the plan. It runs elevated, started by GTG with GTG's
# supervisor as its parent, and starts every build with CreateProcess: never
# through the shell, so no SmartScreen or UAC prompt can stop it.
#
# Windows PowerShell 5.1, the one every supported Windows has. .NET calls only:
# a PowerShell 7 shell can leave its own module path behind, and then Windows
# PowerShell cannot load the commands its modules define.
param([Parameter(Mandatory = $true)][string]$Plan)
$ErrorActionPreference = 'Stop'

$updateDir = Split-Path -Parent $Plan
$logPath = Join-Path $updateDir 'update.log'
$loadedPath = Join-Path $updateDir 'loaded.json'
$readyPath = Join-Path $updateDir 'ready.json'
$outcomePath = Join-Path $updateDir 'outcome.json'

function Log([string]$message) {
    $line = '{0:yyyy-MM-dd HH:mm:ss.fff} pid={1} {2}' -f (Get-Date), $PID, $message
    [IO.File]::AppendAllText($logPath, $line + "`r`n")
}

function Write-Json([string]$path, [hashtable]$values) {
    $temporary = $path + '.tmp'
    [IO.File]::WriteAllText($temporary, ($values | ConvertTo-Json -Compress))
    if ([IO.File]::Exists($path)) { [IO.File]::Delete($path) }
    [IO.File]::Move($temporary, $path)
}

function Get-Sha256([byte[]]$bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-', '') }
    finally { $sha.Dispose() }
}

function Get-Short([string]$build) {
    if ($build.Length -gt 8) { return $build.Substring(0, 8) }
    return $build
}

function Finish([string]$outcome, [string]$reason, $build) {
    Write-Json $outcomePath @{
        outcome = $outcome; reason = $reason; version = $build.version; build = $build.build
        release = $planData.release; reinstall = [bool]$planData.reinstall
        at = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
    }
    try { [IO.File]::Delete($Plan) } catch { }
    Log "outcome=$outcome version=$($build.version) reason=$reason"
    try { $script:mutex.ReleaseMutex() } catch { }
    exit 0
}

# Starts a build and waits for its supervisor to report protection ready, in
# ready.json naming the process this script started.
function Start-Build([string]$what) {
    if ([IO.File]::Exists($readyPath)) { [IO.File]::Delete($readyPath) }
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $entry
    $start.WorkingDirectory = $install
    $start.UseShellExecute = $false
    $process = [Diagnostics.Process]::Start($start)
    $script:started = $process
    Log "started $what pid=$($process.Id)"
    $deadline = [DateTime]::UtcNow.AddMilliseconds([int]$planData.ready_ms)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ([IO.File]::Exists($readyPath)) {
            $report = $null
            try { $report = [IO.File]::ReadAllText($readyPath) | ConvertFrom-Json } catch { }
            if ($report -and [int]$report.pid -eq $process.Id) {
                Log "$what ready pid=$($process.Id)"
                return $true
            }
        }
        if ($process.HasExited) {
            Log "$what pid=$($process.Id) exited code=$($process.ExitCode) before ready"
            return $false
        }
        Start-Sleep -Milliseconds 200
    }
    Log "$what did not report ready within $($planData.ready_ms) ms; stopping pid=$($process.Id)"
    # The supervisor's job is kill-on-close: stopping it stops its Tray too.
    try { $process.Kill(); [void]$process.WaitForExit(5000) } catch { Log "stop failed: $_" }
    return $false
}

function Restart-Old([string]$reason) {
    if (Start-Build 'the old build') { Finish 'aborted' $reason $planData.old }
    Finish 'failed' "$reason; the old build did not come back" $planData.old
}

function Undo-Swap {
    for ($i = $done.Count - 1; $i -ge 0; $i--) {
        $step = $done[$i]
        if ($step.written -and [IO.File]::Exists($step.target)) {
            [IO.File]::Move($step.target, $step.target + '.bad-' + [DateTime]::UtcNow.Ticks)
        }
        if ($step.old) { [IO.File]::Move($step.old, $step.target) }
    }
    Log "undid $($done.Count) file swaps"
    $done.Clear()
}

$script:phase = 'loading'
$script:recovering = $false
$script:started = $null
$script:mutex = $null
$done = New-Object System.Collections.ArrayList

# No error may end this script silently: it has no window, and once GTG has
# exited nothing else will start it again. What to undo depends on how far it
# got. Before loaded.json GTG is still running and waiting for it, so the
# script only stops.
trap {
    $message = $_.Exception.Message
    try { Log "error at line $($_.InvocationInfo.ScriptLineNumber) phase=$($script:phase): $message" } catch { }
    if ($script:phase -eq 'loading') { exit 4 }
    if ($script:recovering) { break }
    $script:recovering = $true
    switch ($script:phase) {
        'waiting' { Finish 'aborted' "error while waiting: $message" $planData.old }
        'done' { Finish 'updated' "ready; cleanup error: $message" $planData.new }
        'rolling_back' { Finish 'failed' "error during rollback: $message" $planData.old }
        'swapping' { try { Undo-Swap } catch { Log "undo failed: $($_.Exception.Message)" } }
        'starting' {
            try { if ($script:started -and -not $script:started.HasExited) { $script:started.Kill() } } catch { }
            try { Undo-Swap } catch { Log "undo failed: $($_.Exception.Message)" }
        }
    }
    Restart-Old "error: $message"
}

# Kept to 256 KiB plus one previous file.
try {
    if ([IO.File]::Exists($logPath) -and (New-Object IO.FileInfo $logPath).Length -gt 262144) {
        [IO.File]::Copy($logPath, $logPath + '.1', $true)
        [IO.File]::Delete($logPath)
    }
} catch { }

# The plan is read once, before anything else. A missing plan means GTG gave up
# waiting for this script and is still running.
if (-not [IO.File]::Exists($Plan)) { exit 2 }
$planData = [IO.File]::ReadAllText($Plan) | ConvertFrom-Json
if ($planData.format -ne 1) { Log "plan format $($planData.format) is not 1"; exit 2 }
$install = $planData.install
$entry = Join-Path $install $planData.entry

# One update at a time, across sessions.
$owned = $false
$script:mutex = [Threading.Mutex]::new($true, $planData.mutex, [ref]$owned)
if (-not $owned) { Log 'another update is running; this one stops'; exit 3 }

Write-Json $loadedPath @{ pid = $PID }
$script:phase = 'waiting'
Log ("plan {0}: {1} {2} -> {3} {4}, {5} files" -f $Plan, $planData.old.version, (Get-Short $planData.old.build),
    $planData.new.version, (Get-Short $planData.new.build), @($planData.files).Count)

# 1. Wait for the supervisor and the Tray to exit. An id whose creation time
# differs was reused by something else.
$deadline = [DateTime]::UtcNow.AddMilliseconds([int]$planData.wait_exit_ms)
foreach ($wait in $planData.wait) {
    $process = $null
    try { $process = [Diagnostics.Process]::GetProcessById([int]$wait.pid) } catch { continue }
    try {
        if ($process.StartTime.ToFileTimeUtc() -ne [long]::Parse($wait.created)) { continue }
    } catch { }
    $left = [int][Math]::Max(0, ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
    if (-not $process.WaitForExit($left)) {
        Finish 'aborted' "pid $($wait.pid) did not exit within $($planData.wait_exit_ms) ms; nothing changed" $planData.old
    }
}
Log 'GTG has exited'
$script:phase = 'checking'

# 2. Read every staged file and check it now: what is installed below is these
# bytes, so nothing can change between this check and the install.
$payload = New-Object System.Collections.ArrayList
foreach ($file in $planData.files) {
    $bytes = [IO.File]::ReadAllBytes($file.staged)
    if ($bytes.Length -ne [long]$file.size -or (Get-Sha256 $bytes) -ne $file.sha256) {
        Restart-Old "staged $($file.name) changed after the plan was written"
    }
    [void]$payload.Add(@{ name = $file.name; bytes = $bytes })
}
Log 'staged files match the plan'

# 3. Swap: a running EXE or a loaded DLL can be renamed, not replaced. Each
# installed file goes aside as .old, and the new one is written in its place.
$script:phase = 'swapping'
foreach ($file in $payload) {
    $target = Join-Path $install $file.name
    $step = @{ target = $target; old = $null; written = $false }
    if ([IO.File]::Exists($target)) {
        $old = $target + '.old'
        if ([IO.File]::Exists($old)) {
            try { [IO.File]::Delete($old) } catch { $old = $target + '.old-' + [DateTime]::UtcNow.Ticks }
        }
        [IO.File]::Move($target, $old)
        $step.old = $old
    }
    [void]$done.Add($step)
    [IO.File]::WriteAllBytes($target, $file.bytes)
    $step.written = $true
}
Log "installed $($done.Count) files"

# 4. Start the new build; put the old one back when it does not report ready.
$script:phase = 'starting'
if (Start-Build 'the new build') {
    $script:phase = 'done'
    foreach ($step in $done) {
        if ($step.old) {
            try { [IO.File]::Delete($step.old) } catch { Log "kept $($step.old): still in use" }
        }
    }
    Finish 'updated' 'ready' $planData.new
}
$script:phase = 'rolling_back'
Undo-Swap
if (Start-Build 'the old build') { Finish 'rolled_back' 'the new build did not report ready' $planData.old }
Finish 'failed' 'the new build did not report ready and the old build did not come back' $planData.old
