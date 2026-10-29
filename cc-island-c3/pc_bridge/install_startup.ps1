param(
    [ValidatePattern('^COM[1-9][0-9]*$')]
    [string]$UsbPort = "COM3",
    [switch]$SkipUsb
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$startupDir = [Environment]::GetFolderPath("Startup")
$targetVbs = Join-Path $startupDir "Codex Usage Tray.vbs"
$trayScript = Join-Path $scriptDir "usage_tray.ps1"

if (-not (Test-Path $trayScript)) {
    Write-Error "Tray script not found: $trayScript"
    exit 1
}

$usbLaunch = ""
if (-not $SkipUsb) {
    $serialScript = Join-Path $scriptDir "send_serial.py"
    if (-not (Test-Path $serialScript)) {
        throw "USB forwarding script not found: $serialScript"
    }
    $pythonExe = (Get-Command python -ErrorAction Stop).Source
    & $pythonExe -c "import serial" 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "USB forwarding requires pyserial. Run: python -m pip install pyserial"
    }
    $pythonwExe = Join-Path (Split-Path $pythonExe -Parent) "pythonw.exe"
    if (-not (Test-Path $pythonwExe)) { $pythonwExe = $pythonExe }
    $UsbPort = $UsbPort.ToUpperInvariant()
    $usbLaunch = @"
If Not IsRunning("$serialScript") Then
    shell.Run """$pythonwExe"" ""$serialScript"" --port $UsbPort", 0, False
End If
"@
}

$vbsContent = @"
Set shell = CreateObject("WScript.Shell")
Set processes = GetObject("winmgmts:\\.\root\cimv2")

Function IsRunning(scriptPath)
    IsRunning = False
    For Each process In processes.ExecQuery("SELECT CommandLine FROM Win32_Process WHERE Name='powershell.exe' OR Name='python.exe' OR Name='pythonw.exe'")
        If Not IsNull(process.CommandLine) Then
            If InStr(1, process.CommandLine, scriptPath, vbTextCompare) > 0 Then
                IsRunning = True
                Exit Function
            End If
        End If
    Next
End Function

If Not IsRunning("$trayScript") Then
    shell.Run "powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File ""$trayScript""", 0, False
End If
$usbLaunch
"@

[System.IO.File]::WriteAllText($targetVbs, $vbsContent, [System.Text.Encoding]::Unicode)
Write-Output "Installed startup launcher:"
Write-Output $targetVbs
if ($SkipUsb) {
    Write-Output "Bridge only (USB forwarding disabled)."
} else {
    Write-Output "Bridge and USB forwarding on $UsbPort."
}
