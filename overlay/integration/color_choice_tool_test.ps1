param([Parameter(Mandatory)][string]$Tool)
$ErrorActionPreference = 'Stop'
$taskCreated = (Get-Process -Id $PID).StartTime.ToUniversalTime().ToFileTimeUtc()
& $Tool '--pid' "$PID" '--created' "$taskCreated" 2>$null
if ($LASTEXITCODE -ne 2) { throw 'Missing explicit assumption was accepted' }
& $Tool '--pid' "$PID" '--created' "$($taskCreated + 1)" '--assume-sdr' 2>$null
if ($LASTEXITCODE -ne 3) { throw 'Wrong target creation was accepted' }
$taskMap = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateNew("Local\GTG.Research.RemoteLoad.$PID", 32)
try {
    $taskProcess = Start-Process -FilePath $Tool -ArgumentList @('--pid', "$PID", '--created', "$taskCreated", '--assume-sdr') -WindowStyle Hidden -PassThru
    if (!$taskProcess.WaitForExit(2000)) { throw 'Resident-hook namespace must refuse a late color choice' }
    if ($taskProcess.ExitCode -ne 6) { throw "Wrong resident refusal code: $($taskProcess.ExitCode)" }
} finally { $taskMap.Dispose() }
# The owned child only targets this test shell. No game or GTG manipulation.
'PASS choice tool explicit flag, creation and resident-session refusal'
