param([Parameter(Mandatory)][string]$Dll, [Parameter(Mandatory)][string]$BaseVersion,
      [Parameter(Mandatory)][int]$Revision)
$ErrorActionPreference = 'Stop'
$taskVersion = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($Dll)
if ($taskVersion.FileVersion -ne "$BaseVersion.$Revision" -or
    $taskVersion.ProductVersion -ne "$BaseVersion.0" -or
    $taskVersion.OriginalFilename -ne 'gtg_overlay.dll' -or
    $taskVersion.FileDescription -ne 'GPU Thermal Guard Game Overlay') {
    throw 'DLL version resource must follow GTG base plus overlay revision.'
}
'PASS overlay file/product version resource'
