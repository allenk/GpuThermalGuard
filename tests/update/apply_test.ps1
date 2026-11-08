# The update script end to end, against stand-ins: the real supervisor code,
# the real hand-off and the script as compiled into the stand-in. Nothing here
# starts GTG; every process it starts lives under the sandbox, with its own
# supervisor singleton and update mutex. Run without elevation by ctest; from
# an elevated shell every start must show elevated=1.
param(
    [Parameter(Mandatory = $true)][string]$Standin,
    [Parameter(Mandatory = $true)][string]$Overlay,
    [string]$Sandbox = (Join-Path ([IO.Path]::GetTempPath()) "gtg-update-apply-$PID"),
    [string[]]$Only
)
$ErrorActionPreference = 'Stop'

$mutexName = 'Local\GtgUpdateStandin.Update'
$env:GTG_STANDIN_MUTEX = $mutexName
$env:GTG_STANDIN_WAIT_MS = '8000'
$env:GTG_STANDIN_READY_MS = '8000'

function Copy-Variant([string]$source, [string]$target, [string]$name) {
    $bytes = [IO.File]::ReadAllBytes($source)
    $marker = [Text.Encoding]::ASCII.GetBytes("`nLABBUILD:$name`n")
    [IO.File]::WriteAllBytes($target, [byte[]]($bytes + $marker))
}

function Get-Marker([string]$file) {
    $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($file))
    $at = $text.LastIndexOf('LABBUILD:')
    if ($at -lt 0) { return 'unmarked' }
    $begin = $at + 9
    return $text.Substring($begin, $text.IndexOf("`n", $begin) - $begin)
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

function New-Install([string]$name, [string]$base) {
    $dir = Join-Path $Sandbox $name
    if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
    New-Item -ItemType Directory -Path (Join-Path $dir '_gtg_update\staged\TEST') | Out-Null
    Copy-Variant $Standin (Join-Path $dir 'app.exe') $base
    Copy-Variant $Overlay (Join-Path $dir 'gtg_overlay.dll') $base
    return $dir
}

function Start-Install([string]$dir, [string]$base) {
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = Join-Path $dir 'app.exe'
    $start.WorkingDirectory = $dir
    $start.UseShellExecute = $false
    [void][Diagnostics.Process]::Start($start)
    $ready = Join-Path $dir 'lab-ready.json'
    if (-not (Wait-Until { $r = Read-Json $ready; $r -and $r.build -eq $base } 10000)) {
        throw "the $base build did not start in $dir"
    }
    # The supervisor logs ready after five healthy observations; give it that.
    Start-Sleep -Milliseconds 1500
    return [int](Read-Json $ready).pid
}

function Send-Request([string]$dir, [string]$flags) {
    $file = Join-Path $dir 'lab-request.json'
    [IO.File]::WriteAllText("$file.tmp", "{`"flags`":`"$flags`"}")
    Move-Item -LiteralPath "$file.tmp" -Destination $file -Force
}

function Test-MutexFree {
    $mutex = $null
    if ([Threading.Mutex]::TryOpenExisting($mutexName, [ref]$mutex)) { $mutex.Dispose(); return $false }
    return $true
}

$results = New-Object System.Collections.ArrayList
function Check([string]$scenario, [string]$what, [bool]$ok, [string]$detail) {
    [void]$results.Add([pscustomobject]@{ Scenario = $scenario; Check = $what; Ok = $ok; Detail = $detail })
}

function Invoke-Scenario {
    param([string]$Name, [string]$Build, [string]$Flags, [scriptblock]$Prepare,
          [string]$Outcome, [string]$Running, [scriptblock]$Extra, [string]$Base = 'base')
    if ($Only -and $Only -notcontains $Name) { return }
    Stop-Sandbox
    $dir = New-Install $Name $Base
    $oldPid = Start-Install $dir $Base
    $staged = Join-Path $dir '_gtg_update\staged\TEST'
    Copy-Variant $Standin (Join-Path $staged 'app.exe') $Build
    Copy-Variant $Overlay (Join-Path $staged 'gtg_overlay.dll') $Build
    if ($Prepare) { & $Prepare $dir }
    $clock = [Diagnostics.Stopwatch]::StartNew()
    Send-Request $dir $Flags
    $work = Join-Path $dir '_gtg_update'
    $outcomeFile = Join-Path $work 'outcome.json'
    if ($Outcome) {
        [void](Wait-Until { Test-Path -LiteralPath $outcomeFile } 60000)
        Start-Sleep -Milliseconds 300
        $result = Read-Json $outcomeFile
        Check $Name 'outcome' ($result -and $result.outcome -eq $Outcome) `
            ("{0} in {1} ms: {2}" -f $result.outcome, $clock.ElapsedMilliseconds, $result.reason)
        $log = Get-Log $dir
        Check $Name 'script outside the job' ($log -match 'handed off script pid=\d+ in_job=0') ''
        Check $Name 'plan removed' (-not (Test-Path -LiteralPath (Join-Path $work 'plan.json'))) ''
        Check $Name 'mutex released' (Wait-Until { Test-MutexFree } 3000) ''
    } else {
        [void](Wait-Until { (Get-Log $dir) -match 'hand-off failed' } 30000)
        Check $Name 'no outcome' (-not (Test-Path -LiteralPath $outcomeFile)) ''
    }
    $ready = Read-Json (Join-Path $dir 'lab-ready.json')
    $alive = $ready -and (Get-Process -Id $ready.pid -ErrorAction SilentlyContinue)
    Check $Name "running build = $Running" ([bool]$alive -and $ready.build -eq $Running) "lab-ready says $($ready.build)"
    $exe = Get-Marker (Join-Path $dir 'app.exe')
    $dll = Get-Marker (Join-Path $dir 'gtg_overlay.dll')
    Check $Name "files = $Running" ($exe -eq $Running -and $dll -eq $Running) "app.exe=$exe gtg_overlay.dll=$dll"
    $count = (Get-Under $dir).Count
    Check $Name 'one supervisor + one tray' ($count -eq 2) "$count processes under the install"
    $elevation = ([regex]::Matches((Get-Log $dir), 'elevated=(\d)') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique) -join ','
    Check $Name 'elevation seen' $true "elevated=$elevation"
    if ($Extra) { & $Extra $dir $oldPid }
}

New-Item -ItemType Directory -Path $Sandbox -Force | Out-Null
try {
    Invoke-Scenario -Name 'update' -Build 'next' -Flags 'go' -Outcome 'updated' -Running 'next' -Extra {
        param($dir)
        $left = @(Get-ChildItem -LiteralPath $dir -Filter '*.old*').Name -join ','
        Check 'update' 'no .old left' (-not $left) $left
        $outcome = Read-Json (Join-Path $dir '_gtg_update\outcome.json')
        Check 'update' 'outcome names the new build' ($outcome.version -eq '1.2.1' -and $outcome.build -eq 'next') "$($outcome.version) $($outcome.build)"
        $log = [IO.File]::ReadAllText((Join-Path $dir '_gtg_update\update.log'))
        Check 'update' 'update.log written' ($log -match 'outcome=updated') ''
    }

    Invoke-Scenario -Name 'crash' -Build 'crash' -Flags 'go' -Outcome 'rolled_back' -Running 'base' -Extra {
        param($dir)
        $bad = @(Get-ChildItem -LiteralPath $dir -Filter '*.bad-*').Count
        Check 'crash' 'new files set aside as .bad' ($bad -eq 2) "$bad files"
    }

    Invoke-Scenario -Name 'tamper' -Build 'next' -Flags 'tamper' -Outcome 'aborted' -Running 'base'

    # No NVIDIA GPU before or after: the new build counts as ready without one.
    Invoke-Scenario -Name 'nogpu' -Base 'nogpu-base' -Build 'nogpu-next' -Flags 'go' `
        -Outcome 'updated' -Running 'nogpu-next'

    # The old build protected a GPU; a new build that finds none is no good.
    Invoke-Scenario -Name 'lost-gpu' -Build 'nogpu-next' -Flags 'go' -Outcome 'rolled_back' `
        -Running 'base'

    Invoke-Scenario -Name 'hang' -Build 'next' -Flags 'hang' -Outcome 'aborted' -Running 'base' -Extra {
        param($dir, $oldPid)
        $same = [bool](Get-Process -Id $oldPid -ErrorAction SilentlyContinue)
        Check 'hang' 'the old tray kept running untouched' $same "pid $oldPid"
    }

    $game = Join-Path $Sandbox 'game'
    Invoke-Scenario -Name 'locked' -Build 'next' -Flags 'go' -Outcome 'updated' -Running 'next' -Prepare {
        param($dir)
        New-Item -ItemType Directory -Path $game -Force | Out-Null
        Copy-Variant $Standin (Join-Path $game 'game.exe') 'game'
        $start = New-Object Diagnostics.ProcessStartInfo
        $start.FileName = Join-Path $game 'game.exe'
        $start.Arguments = "--game `"$(Join-Path $dir 'gtg_overlay.dll')`" 60000"
        $start.UseShellExecute = $false
        [void][Diagnostics.Process]::Start($start)
        if (-not (Wait-Until { (Get-Log $dir) -match 'GAME loaded the overlay' } 5000)) {
            throw 'the game stand-in did not load the overlay'
        }
    } -Extra {
        param($dir)
        $kept = Test-Path -LiteralPath (Join-Path $dir 'gtg_overlay.dll.old')
        Check 'locked' 'loaded DLL renamed aside and kept' $kept 'gtg_overlay.dll.old'
        Check 'locked' 'the game kept running' ((Get-Under $game).Count -eq 1) ''
        Get-Under $game | Stop-Process -Force -ErrorAction SilentlyContinue
    }

    Invoke-Scenario -Name 'motw' -Build 'next' -Flags 'go' -Outcome 'updated' -Running 'next' -Prepare {
        param($dir)
        $exe = Join-Path $dir '_gtg_update\staged\TEST\app.exe'
        Set-Content -LiteralPath $exe -Stream Zone.Identifier -Value "[ZoneTransfer]`r`nZoneId=3"
    } -Extra {
        param($dir)
        $zone = Get-Content -LiteralPath (Join-Path $dir 'app.exe') -Stream Zone.Identifier -ErrorAction SilentlyContinue
        Check 'motw' 'installed app.exe carries no Zone.Identifier' (-not $zone) ($zone -join ' ')
    }

    # Another update holds the mutex: the script stops before loaded.json and
    # the Tray, told so at once, keeps running.
    $held = $null
    Invoke-Scenario -Name 'busy' -Build 'next' -Flags 'go' -Running 'base' -Prepare {
        param($dir)
        $script:held = [Threading.Mutex]::new($true, $mutexName)
    } -Extra {
        param($dir)
        Check 'busy' 'the Tray was told at once' ((Get-Log $dir) -match 'hand-off failed: .*\(code 3\)') ''
        Check 'busy' 'plan removed' (-not (Test-Path -LiteralPath (Join-Path $dir '_gtg_update\plan.json'))) ''
        $script:held.ReleaseMutex()
        $script:held.Dispose()
    }
} finally {
    Stop-Sandbox
}

$results | Format-Table -AutoSize -Wrap | Out-String -Width 220
$failed = @($results | Where-Object { -not $_.Ok })
"{0} checks, {1} failed" -f $results.Count, $failed.Count
if ($failed.Count -eq 0) { Remove-Item -LiteralPath $Sandbox -Recurse -Force -ErrorAction SilentlyContinue }
if ($failed.Count) { exit 1 }
