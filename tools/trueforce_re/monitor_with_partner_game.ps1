#requires -Version 5
<#
.SYNOPSIS
  Monitor TrueForce SDK loading + Available() state while a partner game
  (e.g. Forza Horizon 5) is running.

.DESCRIPTION
  Polls every 3s for ~5 minutes:
    1. Lists every process that has trueforce_sdk_x64.dll loaded.
    2. Runs the standalone trueforce_probe.exe and captures Available()
       state from the first 3 seconds of polling.
    3. Reports state transitions (when SDK loads, when Available() flips).

  Test plan:
    - Run this script from a regular terminal.
    - In another window, launch Forza Horizon 5.
    - Watch the first column flip from "(none)" to "lghub_agent.exe" or
      similar. If Available() also flips to "true", the partner-game-
      gating hypothesis is confirmed: TrueForce only works when a
      registered partner title is running on the same machine.
#>

$probe = Join-Path $PSScriptRoot "..\..\build\tools\trueforce_probe\Release\trueforce_probe.exe"
if (-not (Test-Path $probe)) {
    Write-Host "ERROR: probe exe not found at $probe. Run build.ps1 first." -ForegroundColor Red
    exit 1
}

Write-Host "=== TrueForce partner-game monitor ===" -ForegroundColor Cyan
Write-Host "Polling every 3s for 5 minutes. Launch Forza Horizon 5 now."
Write-Host "Press Ctrl-C to stop early."
Write-Host ""

$lastSdkHosts = ""
$lastAvail    = "?"
$tStart = Get-Date

while (((Get-Date) - $tStart).TotalSeconds -lt 300) {
    $now = (Get-Date).ToString("HH:mm:ss")

    # --- which processes have the SDK loaded ---
    $hosts = (tasklist /m trueforce_sdk_x64.dll 2>&1 |
              Select-String -Pattern "^\S+\s+\d+" |
              ForEach-Object { ($_ -split "\s+")[0] }) -join ", "
    if (-not $hosts) { $hosts = "(none)" }

    # --- run the probe, capture Available() state from the "Probe state"
    #     section. Cast to plain string array so -match operates on text,
    #     not on MatchInfo objects (which coerce to PowerShell's $true
    #     boolean and produce a "True" false positive — bug from earlier
    #     iterations of this script).
    $probeOut = (& $probe 2>&1) | Out-String
    $avail = "?"
    foreach ($line in ($probeOut -split "`r?`n")) {
        if ($line -match "^\s*logiTrueForceAvailable\(\)\s+(true|false|CRASHED)") {
            $avail = $matches[1]
            break
        }
    }

    # --- detect changes ---
    if ($hosts -ne $lastSdkHosts -or $avail -ne $lastAvail) {
        Write-Host "[$now] SDK hosts: $hosts" -ForegroundColor Yellow
        Write-Host "         Available(): $avail" -ForegroundColor Yellow
        if ($avail -eq "true") {
            Write-Host "         *** TrueForce ACTIVATED ***" -ForegroundColor Green
        }
        $lastSdkHosts = $hosts
        $lastAvail    = $avail
    } else {
        Write-Host "[$now] (unchanged) hosts=$hosts  avail=$avail"
    }

    Start-Sleep -Seconds 3
}

Write-Host ""
Write-Host "Monitor finished."
