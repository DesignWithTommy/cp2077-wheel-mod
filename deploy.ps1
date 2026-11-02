# Package or deploy direct_wheel for testing.
#
# Two modes:
#
#   Zip mode (default):
#     Produces dist/direct_wheel-<version>.zip laid out the way the FOMOD expects.
#     Drop that zip on Vortex via File -> Install from file.
#     Example:
#       powershell -ExecutionPolicy Bypass -File deploy.ps1
#
#   Direct mode (-Game <path>):
#     Copies files straight into a Cyberpunk 2077 install directory. Fastest
#     dev loop - no Vortex, no re-zip, no FOMOD prompts.
#     Example:
#       powershell -ExecutionPolicy Bypass -File deploy.ps1 -Game "C:\GOG Games\Cyberpunk 2077"
#
# Common flags:
#   -Config <Debug|Release|RelWithDebInfo>   Which build config to package.
#   -BuildDir <path>                          Where direct_wheel.dll lives (default "build").
#   -Clean                                    Wipe prior artifacts before deploying.
#   -NoBuild                                  Skip invoking build.ps1 even if the DLL is stale.

param(
  [string]$Game,
  [ValidateSet("Debug", "Release", "RelWithDebInfo")]
  [string]$Config = "Release",
  [string]$BuildDir = "build",
  [switch]$Clean,
  [switch]$NoBuild
)

$ErrorActionPreference = "Stop"

$repoRoot = $PSScriptRoot
if (-not $repoRoot) { $repoRoot = (Get-Location).Path }
Set-Location $repoRoot

function Info($msg)    { Write-Host "[deploy] $msg" -ForegroundColor Cyan }
function Ok($msg)      { Write-Host "[deploy] $msg" -ForegroundColor Green }
function Warn($msg)    { Write-Host "[deploy] WARN: $msg" -ForegroundColor Yellow }
function Fail($msg)    {
  Write-Host "[deploy] ERROR: $msg" -ForegroundColor Red
  exit 1
}

# ---------- version ---------------------------------------------------------

$version = "3.4.1"
try {
  $modInfo = Get-Content -Raw "mod_info.json" | ConvertFrom-Json
  if ($modInfo.version) { $version = $modInfo.version }
} catch {
  Warn "Could not read mod_info.json; defaulting version to $version"
}
Info "Version: $version"

# ---------- pre-flight ------------------------------------------------------

$redsFiles = @(
  "direct_wheel_reds\direct_wheel_natives.reds",
  "direct_wheel_reds\direct_wheel_settings.reds",
  "direct_wheel_reds\direct_wheel_mount.reds",
  "direct_wheel_reds\direct_wheel_events.reds",
  "direct_wheel_reds\direct_wheel_surface.reds",
  "direct_wheel_reds\direct_wheel_vehicle_signals.reds"
)
foreach ($r in $redsFiles) {
  if (-not (Test-Path $r)) { Fail "Missing redscript source: $r" }
}

$fomodFiles = @(
  "fomod\info.xml",
  "fomod\ModuleConfig.xml"
)
foreach ($f in $fomodFiles) {
  if (-not (Test-Path $f)) { Fail "Missing FOMOD file: $f" }
}

$fomodConfigFiles = @(
  "fomod_configs\config_handshake_on.json",
  "fomod_configs\config_handshake_off.json"
)
foreach ($f in $fomodConfigFiles) {
  if (-not (Test-Path $f)) { Fail "Missing FOMOD config fragment: $f" }
}



# ---------- build (if needed) ----------------------------------------------

$dllPath = Join-Path $BuildDir "direct_wheel\$Config\direct_wheel.dll"

function Invoke-Build {
  if (-not (Test-Path "build.ps1")) { Fail "build.ps1 missing - can't build." }
  Info "Invoking build.ps1 -Config $Config -BuildDir $BuildDir"
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "build.ps1" -Config $Config -BuildDir $BuildDir
  if ($LASTEXITCODE -ne 0) { Fail "build.ps1 failed (exit $LASTEXITCODE). Fix compile errors before deploying." }
}

if (-not (Test-Path $dllPath)) {
  if ($NoBuild) { Fail "DLL not found at $dllPath and -NoBuild was specified." }
  Warn "DLL not found at $dllPath - running build first"
  Invoke-Build
  if (-not (Test-Path $dllPath)) { Fail "build.ps1 completed but $dllPath still missing." }
} elseif (-not $NoBuild) {
  # Rebuild if any source is newer than the DLL.
  $dllTime = (Get-Item $dllPath).LastWriteTime
  $srcTimes = Get-ChildItem -Recurse -Path "direct_wheel\src","direct_wheel\include","direct_wheel\CMakeLists.txt","CMakeLists.txt" -File |
    Select-Object -ExpandProperty LastWriteTime
  $newest = ($srcTimes | Measure-Object -Maximum).Maximum
  if ($newest -gt $dllTime) {
    Info "Sources newer than DLL - rebuilding"
    Invoke-Build
  } else {
    Info "DLL is up to date; skipping build. Pass -Clean to force."
  }
}

$dllSize = (Get-Item $dllPath).Length
Info "Using DLL: $dllPath ($dllSize bytes)"



# ---------- clean -----------------------------------------------------------

if ($Clean) {
  if (Test-Path "dist")    { Remove-Item -Recurse -Force "dist";    Info "Removed dist/" }
  if (Test-Path "staging") { Remove-Item -Recurse -Force "staging"; Info "Removed staging/" }
}

# ============================================================================
# Direct mode: -Game <path>
# ============================================================================

if ($Game) {
  if (-not (Test-Path $Game)) { Fail "Game path does not exist: $Game" }

  # Sanity: the path should contain bin\x64\Cyberpunk2077.exe.
  $gameExe = Join-Path $Game "bin\x64\Cyberpunk2077.exe"
  if (-not (Test-Path $gameExe)) {
    Fail "Path doesn't look like a Cyberpunk 2077 install (no bin\x64\Cyberpunk2077.exe): $Game"
  }
  Info "Target game install: $Game"

  # Warn if game is running (DLL will be locked).
  $running = Get-Process -Name "Cyberpunk2077" -ErrorAction SilentlyContinue
  if ($running) { Fail "Cyberpunk 2077 is running - close it first (the DLL is locked while the game is open)." }

  # red4ext presence check.
  $red4ext = Join-Path $Game "red4ext\RED4ext.dll"
  if (-not (Test-Path $red4ext)) {
    Warn "RED4ext not detected at $red4ext. The plugin will not load without it."
  }

  # redscript presence check.
  $redscript = Join-Path $Game "engine\tools\scc.exe"
  if (-not (Test-Path $redscript)) {
    Warn "redscript not detected at $redscript. The .reds files will not compile without it."
  }

  # ArchiveXL + Mod Settings presence check (warn-only).
  $archiveXl = Join-Path $Game "red4ext\plugins\ArchiveXL\ArchiveXL.dll"
  if (-not (Test-Path $archiveXl)) {
    Warn "ArchiveXL not detected. Mod Settings depends on it, so the Settings page will not appear."
  }
  $modSettingsDll = Join-Path $Game "red4ext\plugins\mod_settings\mod_settings.dll"
  $modSettingsReds = Join-Path $Game "r6\scripts\mod_settings\ModSettings.reds"
  if (-not (Test-Path $modSettingsDll) -and -not (Test-Path $modSettingsReds)) {
    Warn "Mod Settings not detected (checked red4ext\plugins\mod_settings\ and r6\scripts\mod_settings\). The wheel will work but the in-game Settings page will not appear."
  }

  # Deploy.
  $pluginDir = Join-Path $Game "red4ext\plugins\direct_wheel"
  $scriptDir = Join-Path $Game "r6\scripts\direct_wheel"

  if ($Clean) {
    if (Test-Path $pluginDir) { Remove-Item -Recurse -Force $pluginDir; Info "Removed $pluginDir" }
    if (Test-Path $scriptDir) { Remove-Item -Recurse -Force $scriptDir; Info "Removed $scriptDir" }
  }

  New-Item -ItemType Directory -Force -Path $pluginDir | Out-Null
  New-Item -ItemType Directory -Force -Path $scriptDir | Out-Null

  Copy-Item -Force $dllPath (Join-Path $pluginDir "direct_wheel.dll")
  Info "Deployed DLL -> $(Join-Path $pluginDir 'direct_wheel.dll')"



  # (Legacy: We used to check for Logitech G HUB here, but the mod now uses raw DirectInput and works without it).

  foreach ($r in $redsFiles) {
    $dest = Join-Path $scriptDir (Split-Path $r -Leaf)
    Copy-Item -Force $r $dest
    Info "Deployed reds -> $dest"
  }

  # Invalidate redscript cache so the new .reds files compile on next launch.
  $cache = Join-Path $Game "r6\cache\modded\final.redscripts"
  if (Test-Path $cache) {
    try {
      Remove-Item -Force $cache
      Info "Invalidated redscript cache: $cache (will recompile on next launch)"
    } catch {
      Warn "Could not invalidate redscript cache - next launch may use stale compiled script. File: $cache"
    }
  }

  # Expose the game's live red4ext log dir inside the repo via a directory
  # junction. After one deploy, every subsequent game launch's fresh logs
  # appear immediately under logs/cp2077-logs/ with no copy step - dev
  # tooling can grep/tail them directly.
  $logDir = Join-Path $Game "red4ext\logs"
  $repoRunsDir = Join-Path $repoRoot "logs"
  $junction = Join-Path $repoRunsDir "cp2077-logs"
  if (-not (Test-Path $repoRunsDir)) { New-Item -ItemType Directory -Force -Path $repoRunsDir | Out-Null }
  if (Test-Path $logDir) {
    $reuse = $false
    if (Test-Path $junction) {
      $item = Get-Item $junction -Force
      if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        if ($item.Target -and ($item.Target -ieq $logDir)) {
          $reuse = $true
          Info "Log junction already points at $logDir"
        } else {
          Remove-Item -Force $junction
        }
      } else {
        Warn "logs\cp2077-logs exists but is not a junction - leaving it alone"
        $reuse = $true
      }
    }
    if (-not $reuse) {
      try {
        New-Item -ItemType Junction -Path $junction -Target $logDir | Out-Null
        Ok "Created log junction: $junction -> $logDir"
      } catch {
        Warn "Could not create log junction: $_"
      }
    }
  }

  Write-Host ""
  Ok "Deploy complete."
  Write-Host ""
  Write-Host "Next steps:" -ForegroundColor Cyan
  Write-Host '  1. Launch Cyberpunk 2077. First launch after a .reds change is slow (30 to 60 seconds) due to recompile.'
  Write-Host '  2. Check logs for load confirmation:'
  Write-Host ("       " + (Join-Path $Game 'red4ext\logs\direct_wheel-*.log'))
  Write-Host ("     Look for:   [direct_wheel] loaded v" + $version)
  Write-Host '                 [direct_wheel] wheel bound at SDK slot 0: "G923 ..."'
  Write-Host '                 [direct_wheel:hook] UpdateVehicleCameraInput fired for the first time (live-hook signal once you enter a vehicle)'
  Write-Host '     If the game fails to launch with an RED4ext MessageBox, the UpdateVehicleCameraInput hash did not resolve - update RED4ext itself (its address database ships per game patch).'
  Write-Host '  3. Check redscript compile log (if the Settings page or mount/menu wrappers break):'
  Write-Host ("       " + (Join-Path $Game 'r6\cache\modded\final.redscripts.log'))
  Write-Host '  4. In-game: Main Menu -> Settings -> Mod Settings -> G-series Wheel.'
  exit 0
}

# ============================================================================
# Zip mode (default)
# ============================================================================

$stagingDir = Join-Path $repoRoot "staging"
$distDir    = Join-Path $repoRoot "dist"
$zipPath    = Join-Path $distDir "direct_wheel-$version.zip"

if (Test-Path $stagingDir) { Remove-Item -Recurse -Force $stagingDir }
New-Item -ItemType Directory -Force -Path $stagingDir | Out-Null
New-Item -ItemType Directory -Force -Path $distDir    | Out-Null

# Create standard game directory layout for manual installs.
New-Item -ItemType Directory -Force -Path (Join-Path $stagingDir "red4ext\plugins\direct_wheel") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stagingDir "r6\scripts\direct_wheel")      | Out-Null

# FOMOD metadata and configs (Vortex uses this).
New-Item -ItemType Directory -Force -Path (Join-Path $stagingDir "fomod")         | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stagingDir "fomod_configs") | Out-Null

Copy-Item -Force $dllPath                          (Join-Path $stagingDir "red4ext\plugins\direct_wheel\direct_wheel.dll")

Copy-Item -Force "fomod\info.xml"                  (Join-Path $stagingDir "fomod\info.xml")
Copy-Item -Force "fomod\ModuleConfig.xml"          (Join-Path $stagingDir "fomod\ModuleConfig.xml")
foreach ($c in $fomodConfigFiles) {
  Copy-Item -Force $c (Join-Path $stagingDir $c)
}
foreach ($r in $redsFiles) {
  Copy-Item -Force $r (Join-Path $stagingDir (Split-Path $r -Leaf | ForEach-Object { "r6\scripts\direct_wheel\$_" }))
}

# Include README + CHANGELOG as top-level files so Vortex shows them.
if (Test-Path "README.md")    { Copy-Item -Force "README.md"    $stagingDir }
if (Test-Path "CHANGELOG.md") { Copy-Item -Force "CHANGELOG.md" $stagingDir }

if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path (Join-Path $stagingDir "*") -DestinationPath $zipPath -Force

$zipSize = [Math]::Round((Get-Item $zipPath).Length / 1024, 1)

Write-Host ""
Ok "Package ready: $zipPath ($zipSize KB)"
Write-Host ""
Write-Host "To test in-game:" -ForegroundColor Cyan
Write-Host "  Vortex -> File -> Install from file -> $zipPath"
Write-Host "  Deploy (Vortex auto-prompts), then launch the game."
Write-Host ""
Write-Host "For fast dev iteration, skip Vortex:" -ForegroundColor Cyan
Write-Host "  .\deploy.ps1 -Game `"<path-to-cyberpunk-install>`""

# ============================================================================
# Flight bridge addon zip (optional, requires Let There Be Flight)
# ============================================================================
$bridgeFile = "direct_wheel_reds\direct_wheel_flight_bridge.reds"
if (Test-Path $bridgeFile) {
  $bridgeStagingDir = Join-Path $repoRoot "staging_flight_bridge"
  $bridgeZipPath    = Join-Path $distDir "direct_wheel_flight_bridge-$version.zip"

  if (Test-Path $bridgeStagingDir) { Remove-Item -Recurse -Force $bridgeStagingDir }
  New-Item -ItemType Directory -Force -Path (Join-Path $bridgeStagingDir "r6\scripts\direct_wheel") | Out-Null

  Copy-Item -Force $bridgeFile (Join-Path $bridgeStagingDir "r6\scripts\direct_wheel\direct_wheel_flight_bridge.reds")

  if (Test-Path $bridgeZipPath) { Remove-Item -Force $bridgeZipPath }
  Compress-Archive -Path (Join-Path $bridgeStagingDir "*") -DestinationPath $bridgeZipPath -Force

  $bridgeZipSize = [Math]::Round((Get-Item $bridgeZipPath).Length / 1024, 1)
  Ok "Flight bridge addon: $bridgeZipPath ($bridgeZipSize KB)"
  Remove-Item -Recurse -Force $bridgeStagingDir
}
