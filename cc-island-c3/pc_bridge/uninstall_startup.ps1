$startupDir = [Environment]::GetFolderPath("Startup")
$targetVbs = Join-Path $startupDir "Codex Usage Tray.vbs"

if (Test-Path $targetVbs) {
    Remove-Item -Force $targetVbs
    Write-Output "Removed startup launcher:"
    Write-Output $targetVbs
} else {
    Write-Output "Startup launcher not present:"
    Write-Output $targetVbs
}
