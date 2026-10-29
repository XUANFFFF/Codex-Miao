Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$logPath = Join-Path $scriptDir "usage_tray.log"

function Write-Log {
    param([string]$Message)
    $timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    Add-Content -Path $logPath -Value "[$timestamp] $Message"
}

Write-Log "usage_tray.ps1 starting"

$serveScript = Join-Path $scriptDir "serve_usage.py"
$bridgeModePath = Join-Path $scriptDir "bridge_mode.json"
$pythonCmd = Get-Command python -ErrorAction SilentlyContinue
if (-not $pythonCmd) {
    Write-Log "python not found on PATH"
    [System.Windows.Forms.MessageBox]::Show(
        "Python was not found on PATH.",
        "Usage Tray",
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Error
    ) | Out-Null
    exit 1
}

$pythonExe = $pythonCmd.Source
$pythonwExe = Join-Path (Split-Path $pythonExe -Parent) "pythonw.exe"
if (-not (Test-Path $pythonwExe)) {
    $pythonwExe = $pythonExe
}
Write-Log "using python executable: $pythonwExe"

$bridgeHost = "0.0.0.0"
$bridgePort = 8765

function Get-BridgeDiscoveryDefaults {
    param(
        [string]$PythonPath,
        [string]$BridgeScriptPath
    )

    if (-not (Test-Path $BridgeScriptPath)) {
        Write-Log "serve_usage.py not found; discovery defaults will be left to the Python bridge"
        return $null
    }

    $probeScript = @'
import ast
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
tree = ast.parse(path.read_text(encoding="utf-8"))
values = {}
for node in tree.body:
    if isinstance(node, ast.Assign):
        for target in node.targets:
            if isinstance(target, ast.Name) and target.id in ("DEFAULT_DISCOVERY_PORT", "DEFAULT_DISCOVERY_INTERVAL"):
                values[target.id] = ast.literal_eval(node.value)

print(json.dumps({
    "port": values["DEFAULT_DISCOVERY_PORT"],
    "interval": values["DEFAULT_DISCOVERY_INTERVAL"],
}))
'@

    try {
        $probeOutput = & $PythonPath -c $probeScript $BridgeScriptPath 2>$null
        if ($LASTEXITCODE -ne 0 -or -not $probeOutput) {
            throw "Python probe did not return discovery defaults."
        }

        $parsedDefaults = $probeOutput | ConvertFrom-Json
        if ($null -eq $parsedDefaults.port -or $null -eq $parsedDefaults.interval) {
            throw "Python probe returned incomplete discovery defaults."
        }

        $port = [int]$parsedDefaults.port
        $interval = [double]$parsedDefaults.interval
        $intervalText = $interval.ToString([System.Globalization.CultureInfo]::InvariantCulture)
        Write-Log "loaded discovery defaults from serve_usage.py: port=$port interval=$intervalText"
        return @{
            Port = $port
            Interval = $interval
        }
    } catch {
        Write-Log ("failed to read discovery defaults from serve_usage.py; discovery args will be omitted so Python uses built-in defaults: {0}" -f $_.Exception.Message)
    }

    return $null
}

$bridgeDiscoveryDefaults = Get-BridgeDiscoveryDefaults -PythonPath $pythonExe -BridgeScriptPath $serveScript
$bridgeDiscoveryArgs = ""
if ($bridgeDiscoveryDefaults) {
    $bridgeDiscoveryPort = $bridgeDiscoveryDefaults.Port
    $bridgeDiscoveryIntervalText = $bridgeDiscoveryDefaults.Interval.ToString([System.Globalization.CultureInfo]::InvariantCulture)
    $bridgeDiscoveryArgs = " --discovery-port $bridgeDiscoveryPort --discovery-interval $bridgeDiscoveryIntervalText"
}
$bridgeUrl = "http://127.0.0.1:$bridgePort/usage"
$bridgeHealthUrl = "http://127.0.0.1:$bridgePort/health"
$bridgeProcess = $null
$bridgeRunning = $false
$bridgePaused = $false
$lastLoggedHealthState = ""

function Set-BridgeMode {
    param([string]$Mode)
    $payload = @{ mode = $Mode } | ConvertTo-Json -Compress
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($bridgeModePath, $payload, $utf8NoBom)
    Write-Log "bridge mode set to $Mode"
}

function Get-BridgeMode {
    if (-not (Test-Path $bridgeModePath)) {
        return "live"
    }

    try {
        $data = Get-Content $bridgeModePath -Raw | ConvertFrom-Json
        if ($data.mode) {
            return [string]$data.mode
        }
    } catch {
        Write-Log "bridge mode read failed: $($_.Exception.Message)"
    }

    return "live"
}

function Test-BridgeHealthy {
    try {
        $resp = Invoke-WebRequest -Uri $bridgeHealthUrl -UseBasicParsing -TimeoutSec 3
        return $resp.StatusCode -eq 200
    } catch {
        return $false
    }
}

function Start-BridgeProcess {
    if ($script:bridgeProcess -and -not $script:bridgeProcess.HasExited) {
        Write-Log "bridge process already running"
        return $true
    }

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $pythonwExe
    $psi.Arguments = "`"$serveScript`" --host $bridgeHost --port $bridgePort$bridgeDiscoveryArgs --mode-path `"$bridgeModePath`""
    $psi.WorkingDirectory = $scriptDir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden

    try {
        $script:bridgeProcess = [System.Diagnostics.Process]::Start($psi)
        Start-Sleep -Milliseconds 600
        Write-Log "bridge process started"
        return $true
    } catch {
        Write-Log "failed to start bridge: $($_.Exception.Message)"
        [System.Windows.Forms.MessageBox]::Show(
            "Failed to start usage bridge.`n$($_.Exception.Message)",
            "Usage Tray",
            [System.Windows.Forms.MessageBoxButtons]::OK,
            [System.Windows.Forms.MessageBoxIcon]::Error
        ) | Out-Null
        return $false
    }
}

function Stop-BridgeProcess {
    if ($script:bridgeProcess -and -not $script:bridgeProcess.HasExited) {
        try {
            $script:bridgeProcess.Kill()
            $script:bridgeProcess.WaitForExit(1500) | Out-Null
            Write-Log "bridge process stopped"
        } catch {
            Write-Log "bridge stop error: $($_.Exception.Message)"
        }
    }
    $script:bridgeProcess = $null
}

function Update-BridgeState {
    $healthy = Test-BridgeHealthy
    if (-not $healthy -and $script:bridgeProcess -and $script:bridgeProcess.HasExited) {
        $script:bridgeProcess = $null
    }
    $script:bridgeRunning = $healthy
    $script:bridgePaused = $healthy -and (Get-BridgeMode) -eq "offline"
    $healthLabel = if (-not $healthy) { "stopped" } elseif ($script:bridgePaused) { "paused" } else { "running" }
    if ($healthLabel -ne $script:lastLoggedHealthState) {
        Write-Log "bridge state=$healthLabel"
        $script:lastLoggedHealthState = $healthLabel
    }
}

function New-TrayIcon {
    $bitmap = New-Object System.Drawing.Bitmap 16,16
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.Clear([System.Drawing.Color]::Black)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias

    $whiteBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
    $cyanBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::Cyan)
    $pinkBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::DeepPink)

    $graphics.FillEllipse($whiteBrush, 2, 3, 4, 6)
    $graphics.FillEllipse($whiteBrush, 10, 3, 4, 6)
    $graphics.FillEllipse($cyanBrush, 4, 4, 1, 1)
    $graphics.FillEllipse($cyanBrush, 11, 4, 1, 1)
    $graphics.DrawArc((New-Object System.Drawing.Pen([System.Drawing.Color]::White, 1.2)), 5, 8, 6, 3, 0, 180)
    $graphics.FillEllipse($pinkBrush, 1, 9, 2, 2)
    $graphics.FillEllipse($pinkBrush, 13, 9, 2, 2)

    $hIcon = $bitmap.GetHicon()
    $icon = [System.Drawing.Icon]::FromHandle($hIcon)
    $graphics.Dispose()
    $whiteBrush.Dispose()
    $cyanBrush.Dispose()
    $pinkBrush.Dispose()
    return @{ Icon = $icon; Handle = $hIcon; Bitmap = $bitmap }
}

$trayAssets = New-TrayIcon
Write-Log "tray icon assets created"
$notifyIcon = New-Object System.Windows.Forms.NotifyIcon
$notifyIcon.Icon = $trayAssets.Icon
$notifyIcon.Text = "Codex Usage Bridge"
$notifyIcon.Visible = $true
Write-Log "notify icon visible set"

$contextMenu = New-Object System.Windows.Forms.ContextMenuStrip
$statusItem = $contextMenu.Items.Add("Status: starting")
$statusItem.Enabled = $false
$contextMenu.Items.Add("-") | Out-Null

$restartItem = $contextMenu.Items.Add("Restart Bridge")
$restartItem.Add_Click({
    Stop-BridgeProcess
    Set-BridgeMode "live"
    if (Start-BridgeProcess) {
        Update-BridgeState
        Update-TrayPresentation
    }
})

$openItem = $contextMenu.Items.Add("Open Bridge URL")
$openItem.Add_Click({
    Start-Process $bridgeUrl | Out-Null
})

$stopItem = $contextMenu.Items.Add("Stop Bridge")
$stopItem.Add_Click({
    Set-BridgeMode "offline"
    if (-not $script:bridgeProcess -or $script:bridgeProcess.HasExited) {
        Start-BridgeProcess | Out-Null
    }
    Update-BridgeState
    Update-TrayPresentation
})

$startItem = $contextMenu.Items.Add("Start Bridge")
$startItem.Add_Click({
    Set-BridgeMode "live"
    if (Start-BridgeProcess) {
        Update-BridgeState
        Update-TrayPresentation
    }
})

$contextMenu.Items.Add("-") | Out-Null
$exitItem = $contextMenu.Items.Add("Exit")
$appContext = New-Object System.Windows.Forms.ApplicationContext

$exitItem.Add_Click({
    $timer.Stop()
    Set-BridgeMode "live"
    Stop-BridgeProcess
    $notifyIcon.Visible = $false
    $appContext.ExitThread()
})

$notifyIcon.ContextMenuStrip = $contextMenu
$notifyIcon.Add_DoubleClick({
    Start-Process $bridgeUrl | Out-Null
})

function Update-TrayPresentation {
    if ($script:bridgeRunning -and -not $script:bridgePaused) {
        $statusItem.Text = "Status: running"
        $notifyIcon.Text = "Codex Usage Bridge - running"
        $startItem.Enabled = $false
        $stopItem.Enabled = $true
        $openItem.Enabled = $true
    } elseif ($script:bridgeRunning) {
        $statusItem.Text = "Status: paused"
        $notifyIcon.Text = "Codex Usage Bridge - paused"
        $startItem.Enabled = $true
        $stopItem.Enabled = $false
        $openItem.Enabled = $true
    } else {
        $statusItem.Text = "Status: stopped"
        $notifyIcon.Text = "Codex Usage Bridge - stopped"
        $startItem.Enabled = $true
        $stopItem.Enabled = $false
        $openItem.Enabled = $false
    }
}

Set-BridgeMode "live"
if (Start-BridgeProcess) {
    Update-BridgeState
}
Update-TrayPresentation

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 3000
$timer.Add_Tick({
    $before = if (-not $script:bridgeRunning) { "stopped" } elseif ($script:bridgePaused) { "paused" } else { "running" }
    Update-BridgeState
    Update-TrayPresentation
    $after = if (-not $script:bridgeRunning) { "stopped" } elseif ($script:bridgePaused) { "paused" } else { "running" }
    if ($before -ne $after) {
        if ($after -eq "running") {
            $notifyIcon.ShowBalloonTip(1500, "Usage Bridge", "Bridge is running.", [System.Windows.Forms.ToolTipIcon]::Info)
        } elseif ($after -eq "paused") {
            $notifyIcon.ShowBalloonTip(1500, "Usage Bridge", "Bridge is paused.", [System.Windows.Forms.ToolTipIcon]::Info)
        } else {
            $notifyIcon.ShowBalloonTip(1500, "Usage Bridge", "Bridge stopped.", [System.Windows.Forms.ToolTipIcon]::Warning)
        }
    }
})
$timer.Start()

try {
    $notifyIcon.ShowBalloonTip(2000, "Usage Bridge", "Tray is running.", [System.Windows.Forms.ToolTipIcon]::Info)
    Write-Log "entering Application.Run"
    [System.Windows.Forms.Application]::Run($appContext)
} finally {
    Write-Log "tray shutting down"
    $timer.Stop()
    Stop-BridgeProcess
    $notifyIcon.Visible = $false
    $notifyIcon.Dispose()
    $trayAssets.Bitmap.Dispose()
}
