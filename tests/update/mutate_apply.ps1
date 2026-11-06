# Injected defects in the update script: each must turn its scenario in
# apply_test.ps1 red. A defect that stays green is one the tests cannot see.
param(
    [Parameter(Mandatory = $true)][string]$Standin,
    [Parameter(Mandatory = $true)][string]$Overlay
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$original = [IO.File]::ReadAllText((Join-Path $here '..\..\src\update\apply-update.ps1'))
$nl = if ($original.Contains("`r`n")) { "`r`n" } else { "`n" }
$mutants = @(
    @{ name = 'no hash re-check'; scenario = 'tamper'
       find = 'if ($bytes.Length -ne [long]$file.size -or (Get-Sha256 $bytes) -ne $file.sha256) {'
       replace = 'if ($false) {' }
    @{ name = 'no rollback'; scenario = 'crash'
       find = "`$script:phase = 'rolling_back'${nl}Undo-Swap"
       replace = "Finish 'failed' 'no rollback' `$planData.old" }
    @{ name = 'loaded before the mutex'; scenario = 'busy'
       find = "`$owned = `$false"
       replace = "Write-Json `$loadedPath @{ pid = `$PID }${nl}`$owned = `$false" }
    @{ name = 'copy instead of bytes'; scenario = 'motw'
       find = '[IO.File]::WriteAllBytes($target, $file.bytes)'
       replace = '[IO.File]::Copy(($planData.files | Where-Object name -eq $file.name).staged, $target)' }
    @{ name = 'no wait for exit'; scenario = 'hang'
       find = 'if (-not $process.WaitForExit($left)) {'
       replace = 'if ($false) {' }
)
$work = Join-Path ([IO.Path]::GetTempPath()) "gtg-update-mutants-$PID"
New-Item -ItemType Directory -Path $work -Force | Out-Null
$silent = 0
try {
    foreach ($mutant in $mutants) {
        if (-not $original.Contains($mutant.find)) { throw "mutant '$($mutant.name)' does not apply" }
        $file = Join-Path $work 'apply-update.ps1'
        [IO.File]::WriteAllText($file, $original.Replace($mutant.find, $mutant.replace))
        $env:GTG_STANDIN_SCRIPT = $file
        $output = & (Join-Path $here 'apply_test.ps1') -Standin $Standin -Overlay $Overlay -Only $mutant.scenario `
            -Sandbox (Join-Path $work 'sandbox') 2>&1 | Out-String -Width 220
        $line = ($output -split "`n" | Where-Object { $_ -match 'checks, \d+ failed' }) -join ''
        $red = $line -notmatch ', 0 failed'
        if (-not $red) { $silent++ }
        '{0} {1,-24} {2,-7} {3}' -f $(if ($red) { 'red ' } else { 'MISS' }), $mutant.name, $mutant.scenario, $line.Trim()
    }
} finally {
    Remove-Item Env:GTG_STANDIN_SCRIPT -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
"{0} of {1} defects went unseen" -f $silent, $mutants.Count
if ($silent) { exit 1 }
