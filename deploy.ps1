# Package or deploy gwheel for testing.
#
# Two modes:
#
#   Zip mode (default):
#     Produces dist/gwheel-<version>.zip laid out the way the FOMOD expects.
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
#   -BuildDir <path>                          Where gwheel.dll lives (default "build").
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

$version = "2.31.0"
try {
  $modInfo = Get-Content -Raw "mod_info.json" | ConvertFrom-Json
  if ($modInfo.version) { $version = $modInfo.version }
} catch {
  Warn "Could not read mod_info.json; defaulting version to $version"
}
Info "Version: $version"

# ---------- pre-flight ------------------------------------------------------

$redsFiles = @(
  "gwheel_reds\gwheel_natives.reds",
  "gwheel_reds\gwheel_settings.reds",
  "gwheel_reds\gwheel_mount.reds",
  "gwheel_reds\gwheel_events.reds",
  "gwheel_reds\gwheel_surface.reds",
  "gwheel_reds\gwheel_vehicle_signals.reds",
  "gwheel_reds\gwheel_input_provider.reds"
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

# Patched mod_settings.dll fork (vendor/mod_settings/). Built by build.ps1
# alongside gwheel.dll; the existence check happens after Invoke-Build below.
$patchedModSettingsDll = Join-Path $repoRoot "vendor\mod_settings\build\Release\mod_settings.dll"

# ---------- build (if needed) ----------------------------------------------

$dllPath = Join-Path $BuildDir "gwheel\$Config\gwheel.dll"

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
  $srcTimes = Get-ChildItem -Recurse -Path "gwheel\src","gwheel\include","gwheel\CMakeLists.txt","CMakeLists.txt" -File |
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

# Sanity-check the patched mod_settings.dll. build.ps1 should have produced
# it as a side effect; if it's missing now, build.ps1 has a bug.
if (-not (Test-Path $patchedModSettingsDll)) {
  Fail "Patched mod_settings.dll missing at $patchedModSettingsDll after build. build.ps1 should have produced it."
}
$msDllSize = (Get-Item $patchedModSettingsDll).Length
Info "Using patched mod_settings.dll: $patchedModSettingsDll ($msDllSize bytes)"

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
  $pluginDir = Join-Path $Game "red4ext\plugins\gwheel"
  $scriptDir = Join-Path $Game "r6\scripts\gwheel"

  if ($Clean) {
    if (Test-Path $pluginDir) { Remove-Item -Recurse -Force $pluginDir; Info "Removed $pluginDir" }
    if (Test-Path $scriptDir) { Remove-Item -Recurse -Force $scriptDir; Info "Removed $scriptDir" }
  }

  New-Item -ItemType Directory -Force -Path $pluginDir | Out-Null
  New-Item -ItemType Directory -Force -Path $scriptDir | Out-Null

  Copy-Item -Force $dllPath (Join-Path $pluginDir "gwheel.dll")
  Info "Deployed DLL -> $(Join-Path $pluginDir 'gwheel.dll')"

  # Patched mod_settings DLL: $patchedModSettingsDll resolved + existence-checked
  # in pre-flight above. Overwrite the user's existing mod_settings.dll in place;
  # the patch is API-compatible with upstream so other mods using mod_settings
  # keep working.
  $modSettingsTarget = Join-Path $Game "red4ext\plugins\mod_settings\mod_settings.dll"
  $modSettingsDir    = Split-Path $modSettingsTarget -Parent
  if (-not (Test-Path $modSettingsDir)) {
    Warn "mod_settings folder doesn't exist at $modSettingsDir - install jackhumbert/mod_settings first, then re-run."
  } else {
    Copy-Item -Force $patchedModSettingsDll $modSettingsTarget
    Info "Deployed patched mod_settings.dll -> $modSettingsTarget"

    # Ship the patched mod_settings packed.reds (our fork's redscript-side
    # changes — InputBinding primitive, InputBindingDisplay resolver, the
    # listening UI capture loop). Built by build.ps1 / the manual
    # PackFiles.cmake step. Without this copy, redscript-side fixes never
    # reach the game (mod_settings.dll is C++ only and the user's existing
    # packed.reds from upstream stays in place).
    $patchedModSettingsReds = Join-Path $repoRoot "vendor\mod_settings\build\staging\packed.reds"
    if (Test-Path $patchedModSettingsReds) {
      Copy-Item -Force $patchedModSettingsReds (Join-Path $modSettingsDir "packed.reds")
      Info "Deployed patched mod_settings packed.reds -> $modSettingsDir\packed.reds"
    } else {
      Warn "No staged packed.reds at $patchedModSettingsReds. Run build.ps1 (or manually invoke vendor/mod_settings/deps/cpcmake/scripts/PackFiles.cmake) before deploy."
    }

    # Ship the Wolvenkit-packed ModSettings.archive (visual layout edits to
    # mod_settings_main.inkwidget, etc.) and its .archive.xl companion
    # (localization registration).
    #
    # Read directly from src/wolvenkit/packed/, NOT from the game_dir/
    # staging copy. The staging copy is populated by mod_settings's CMake
    # post-build target, which silently fails (LASTEXITCODE=1, swallowed
    # by build.ps1) whenever Wolvenkit's "Pack Mod" path doesn't match
    # what CMake expects. Reading from packed/ removes one layer of
    # silent staleness — when the user clicks "Pack Mod" in the Wolvenkit
    # GUI, the next deploy picks it up directly. The .archive.xl source
    # is the authored file at src/archiveXL/, which Wolvenkit's pack also
    # copies into game_dir/ but we don't need that hop.
    $packedArchiveSrc   = Join-Path $repoRoot "vendor\mod_settings\src\wolvenkit\packed\archive\pc\mod\ModSettings.archive"
    $packedArchiveXlSrc = Join-Path $repoRoot "vendor\mod_settings\src\archiveXL\ModSettings.archive.xl"
    if (-not (Test-Path $packedArchiveSrc)) {
      Warn "No Wolvenkit-packed archive at $packedArchiveSrc. In Wolvenkit: open ModSettings.cpmodproj, then click Pack Project on the toolbar. Skipping archive deploy."
    } else {
      Copy-Item -Force $packedArchiveSrc (Join-Path $modSettingsDir "ModSettings.archive")
      Info "Deployed ModSettings.archive -> $modSettingsDir\ModSettings.archive"
      if (Test-Path $packedArchiveXlSrc) {
        Copy-Item -Force $packedArchiveXlSrc (Join-Path $modSettingsDir "ModSettings.archive.xl")
        Info "Deployed ModSettings.archive.xl -> $modSettingsDir\ModSettings.archive.xl"
      } else {
        Warn "Authored .archive.xl missing at $packedArchiveXlSrc; skipping."
      }
    }
  }

  # Logitech Gaming Software / G HUB runtime check. The Logitech SDK the DLL
  # is built against talks to either LGS or G HUB through a shared user-mode
  # service; without it LogiSteeringInitialize will fail in-process and the
  # pump thread will log retry warnings.
  $ghub = Get-Process -Name "lghub","LCore" -ErrorAction SilentlyContinue
  if (-not $ghub) {
    Warn "Neither G HUB (lghub.exe) nor LGS (LCore.exe) is running. Start one before launching CP2077."
  } else {
    Info "Logitech runtime detected: $($ghub[0].ProcessName)"
  }

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
  Write-Host ("       " + (Join-Path $Game 'red4ext\logs\gwheel-*.log'))
  Write-Host ("     Look for:   [gwheel] loaded v" + $version)
  Write-Host '                 [gwheel] Logitech Steering Wheel SDK v...'
  Write-Host '                 [gwheel] wheel bound at SDK slot 0: "G923 ..."'
  Write-Host '                 [gwheel] firing gwheel handshake (LED sweep + 4 triplets + centering breath)'
  Write-Host '                 [gwheel:hook] UpdateVehicleCameraInput fired for the first time (live-hook signal once you enter a vehicle)'
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
$zipPath    = Join-Path $distDir "gwheel-$version.zip"

if (Test-Path $stagingDir) { Remove-Item -Recurse -Force $stagingDir }
New-Item -ItemType Directory -Force -Path $stagingDir | Out-Null
New-Item -ItemType Directory -Force -Path $distDir    | Out-Null

# Layout for the FOMOD - source paths match what ModuleConfig.xml references.
New-Item -ItemType Directory -Force -Path (Join-Path $stagingDir "build")         | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stagingDir "fomod")         | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stagingDir "fomod_configs") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stagingDir "gwheel_reds")   | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stagingDir "mod_settings")  | Out-Null

Copy-Item -Force $dllPath                          (Join-Path $stagingDir "build\gwheel.dll")
Copy-Item -Force $patchedModSettingsDll            (Join-Path $stagingDir "mod_settings\mod_settings.dll")
Copy-Item -Force "fomod\info.xml"                  (Join-Path $stagingDir "fomod\info.xml")
Copy-Item -Force "fomod\ModuleConfig.xml"          (Join-Path $stagingDir "fomod\ModuleConfig.xml")
foreach ($c in $fomodConfigFiles) {
  Copy-Item -Force $c (Join-Path $stagingDir $c)
}
foreach ($r in $redsFiles) {
  Copy-Item -Force $r (Join-Path $stagingDir (Split-Path $r -Leaf | ForEach-Object { "gwheel_reds\$_" }))
}

# Patched mod_settings redscript bundle + Wolvenkit-packed archive. These
# carry our fork's redscript-side changes (InputBinding primitive,
# ModSettingsInputProvider, pair-widget UI) and the .inkwidget layout
# edits. Without them the new bind-from-Mod-Settings UI never compiles
# and the pair widget never appears in-game.
$patchedModSettingsReds = Join-Path $repoRoot "vendor\mod_settings\build\staging\packed.reds"
if (-not (Test-Path $patchedModSettingsReds)) {
  Fail "Missing patched mod_settings packed.reds at $patchedModSettingsReds. Run build.ps1 (it packs the bundle as part of the mod_settings build step)."
}
Copy-Item -Force $patchedModSettingsReds (Join-Path $stagingDir "mod_settings\packed.reds")

$packedArchiveSrc   = Join-Path $repoRoot "vendor\mod_settings\src\wolvenkit\packed\archive\pc\mod\ModSettings.archive"
$packedArchiveXlSrc = Join-Path $repoRoot "vendor\mod_settings\src\archiveXL\ModSettings.archive.xl"
if (-not (Test-Path $packedArchiveSrc)) {
  Fail "Missing Wolvenkit-packed ModSettings.archive at $packedArchiveSrc. In Wolvenkit: open ModSettings.cpmodproj, then click Pack Project on the toolbar before re-running deploy."
}
Copy-Item -Force $packedArchiveSrc (Join-Path $stagingDir "mod_settings\ModSettings.archive")
if (-not (Test-Path $packedArchiveXlSrc)) {
  Fail "Missing ModSettings.archive.xl at $packedArchiveXlSrc."
}
Copy-Item -Force $packedArchiveXlSrc (Join-Path $stagingDir "mod_settings\ModSettings.archive.xl")

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
