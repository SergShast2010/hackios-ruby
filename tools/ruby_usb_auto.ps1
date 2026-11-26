# Auto-attach HakiOS USB ECM (VID 1209 / PID 0003) to WSL2 whenever ruby boots.
# Run this in PowerShell BEFORE rebooting the phone, and leave it open.
param(
    [string]$Distro = "Debian",
    [string]$VidPid = "VID_1209&PID_0003",
    [int]$PollMs = 500,
    [switch]$Once
)

$ErrorActionPreference = "Continue"

function Stamp { (Get-Date).ToString("HH:mm:ss") }

Write-Host "[$(Stamp)] Watching for HakiOS USB ECM ($VidPid) -> WSL '$Distro' (Ctrl+C to stop)..." -ForegroundColor Cyan

$lastState = ""
while ($true) {
    try {
        $raw = & usbipd state 2>$null
        if (-not $raw) {
            Start-Sleep -Milliseconds $PollMs
            continue
        }
        $st = $raw | ConvertFrom-Json
        $dev = $st.Devices | Where-Object {
            $_.InstanceId -and ($_.InstanceId.ToUpper().Contains($VidPid.ToUpper())) -and $_.BusId
        } | Select-Object -First 1

        if (-not $dev) {
            if ($lastState -ne "waiting") {
                if ($lastState -ne "") {
                    Write-Host "[$(Stamp)] Device disconnected; waiting for next boot..." -ForegroundColor DarkGray
                }
                $lastState = "waiting"
            }
            Start-Sleep -Milliseconds $PollMs
            continue
        }

        if ($dev.ClientIPAddress) {
            if ($lastState -ne "attached:$($dev.BusId)") {
                Write-Host "[$(Stamp)] Attached busid $($dev.BusId) ($($dev.Description)) -> WSL $($dev.ClientIPAddress)" -ForegroundColor Green
                $lastState = "attached:$($dev.BusId)"
                if ($Once) { break }
            }
            Start-Sleep -Milliseconds $PollMs
            continue
        }

        if (-not $dev.PersistedGuid) {
            Write-Host "[$(Stamp)] Found busid $($dev.BusId) (Not shared) -> running usbipd bind..." -ForegroundColor Yellow
            & usbipd bind --busid $dev.BusId
            if ($LASTEXITCODE -ne 0) {
                Write-Host "[$(Stamp)] usbipd bind failed (run PowerShell as Administrator once to bind)." -ForegroundColor Red
                Start-Sleep -Seconds 3
                continue
            }
        }

        Write-Host "[$(Stamp)] Found HakiOS USB ECM on busid $($dev.BusId) -> attaching to WSL '$Distro'..." -ForegroundColor Yellow
        & usbipd attach --wsl $Distro --busid $dev.BusId
        if ($LASTEXITCODE -eq 0) {
            Write-Host "[$(Stamp)] Attached busid $($dev.BusId) -> WSL '$Distro' OK!" -ForegroundColor Green
            $lastState = "attached:$($dev.BusId)"
            if ($Once) { break }
        } else {
            Write-Host "[$(Stamp)] usbipd attach returned $LASTEXITCODE; retrying..." -ForegroundColor Red
            Start-Sleep -Seconds 1
        }
    } catch {
        Write-Host "[$(Stamp)] Error: $_" -ForegroundColor Red
        Start-Sleep -Seconds 1
    }
    Start-Sleep -Milliseconds $PollMs
}
