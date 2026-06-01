$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$startupDir = [Environment]::GetFolderPath("Startup")
$targetVbs = Join-Path $startupDir "Codex Usage Tray.vbs"
$trayScript = Join-Path $scriptDir "usage_tray.ps1"

if (-not (Test-Path $trayScript)) {
    Write-Error "Tray script not found: $trayScript"
    exit 1
}

$vbsContent = @"
Set shell = CreateObject("WScript.Shell")
shell.Run "powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File ""$trayScript""", 0, False
"@

Set-Content -Path $targetVbs -Value $vbsContent -Encoding ASCII
Write-Output "Installed startup launcher:"
Write-Output $targetVbs
