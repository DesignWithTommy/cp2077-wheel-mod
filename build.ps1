# Build the project: patched mod_settings.dll (vendor/mod_settings/) followed by
# gwheel.dll itself.
#
# Requires:
#   - Visual Studio 2022 with the "Desktop development with C++" workload
#   - CMake 3.21+ on PATH, OR the "C++ CMake tools for Windows" VS workload
#     (which bundles a cmake.exe under the VS install dir)
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File build.ps1
#   powershell -ExecutionPolicy Bypass -File build.ps1 -Config Debug

param(
  [ValidateSet("Debug", "Release", "RelWithDebInfo")]
  [string]$Config = "Release",

  [string]$BuildDir = "build"
)

$ErrorActionPreference = "Stop"

function Resolve-CMake {
  $candidates = @(
    (Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -First 1).Source,
    "${Env:ProgramFiles}\CMake\bin\cmake.exe",
    "${Env:ProgramFiles}\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "${Env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "${Env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  )
  foreach ($c in $candidates) {
    if ($c -and (Test-Path $c)) { return $c }
  }
  throw "CMake not found. Install CMake 3.21+ or add the 'C++ CMake tools for Windows' VS workload."
}

$cmake = Resolve-CMake
Write-Host "Using CMake: $cmake"

# ---------- submodule init -------------------------------------------------
# Pulls vendor/RED4ext.SDK and vendor/mod_settings (and mod_settings' own
# nested deps: spdlog, Detours, RED4ext.SDK fork, cpcmake, archive_xl,
# red_lib). Recursive so the second-level submodules under vendor/mod_settings
# also resolve in one pass.

if (-not (Test-Path "vendor\RED4ext.SDK\CMakeLists.txt")) {
  Write-Host "Initializing top-level submodules..."
  git submodule update --init --recursive
  if ($LASTEXITCODE -ne 0) { throw "git submodule update --init --recursive failed." }
}
if (-not (Test-Path "vendor\mod_settings\CMakeLists.txt")) {
  Write-Host "Initializing vendor/mod_settings submodule..."
  git submodule update --init --recursive vendor/mod_settings
  if ($LASTEXITCODE -ne 0) { throw "git submodule update --init --recursive vendor/mod_settings failed." }
}
# Even if vendor/mod_settings/ is checked out, its own nested submodules
# (deps/) may not be. Force-init them.
if (-not (Test-Path "vendor\mod_settings\deps\red4ext.sdk\CMakeLists.txt")) {
  Write-Host "Initializing vendor/mod_settings nested submodules..."
  git -C vendor/mod_settings submodule update --init --recursive
  if ($LASTEXITCODE -ne 0) { throw "nested submodule init in vendor/mod_settings failed." }
}

# ---------- patched mod_settings.dll ----------------------------------------
# vendor/mod_settings/ is our fork of jackhumbert/mod_settings with a
# `ModSettings.hidden` runtime property + empty-category UI filter.
# Builds via its own CMake project; output lands in
# vendor/mod_settings/build/Release/mod_settings.dll where deploy.ps1
# expects it.

$msDll       = "vendor\mod_settings\build\Release\mod_settings.dll"
$msBuildDir  = "vendor\mod_settings\build"
$msNeedBuild = -not (Test-Path $msDll)
if (-not $msNeedBuild) {
  $msDllTime = (Get-Item $msDll).LastWriteTime
  $msSrcTimes = Get-ChildItem -Recurse -Path "vendor\mod_settings\src","vendor\mod_settings\include","vendor\mod_settings\CMakeLists.txt" -File -ErrorAction SilentlyContinue |
    Select-Object -ExpandProperty LastWriteTime
  if ($msSrcTimes) {
    $msNewest = ($msSrcTimes | Measure-Object -Maximum).Maximum
    if ($msNewest -gt $msDllTime) { $msNeedBuild = $true }
  }
}
if ($msNeedBuild) {
  Write-Host "Building patched mod_settings.dll (vendor/mod_settings/)..."
  # The vendored mod_settings cmake project has a post-build step that
  # copy_if_different's the DLL/PDB into game_dir/ and game_dir_debug/.
  # If those target directories don't exist (Wolvenkit's pack workflow
  # sometimes wipes them, fresh clone never created them), the copy
  # fails MSBuild and the whole build aborts. Pre-create idempotently
  # so a clean checkout always builds.
  foreach ($d in @(
      "vendor\mod_settings\game_dir\red4ext\plugins\mod_settings",
      "vendor\mod_settings\game_dir_debug\red4ext\plugins\mod_settings")) {
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
  }
  & $cmake -S vendor/mod_settings -B $msBuildDir -A x64
  if ($LASTEXITCODE -ne 0) { throw "mod_settings CMake configure failed." }
  & $cmake --build $msBuildDir --config Release
  # If the post-build copy step fails (e.g. the game_dir target paths
  # were wiped by an outside step like Wolvenkit's Pack Mod), don't
  # abort the whole build — as long as the DLL itself was produced,
  # we can continue and the pack/deploy steps will still run.
  if (-not (Test-Path $msDll)) { throw "mod_settings build did not produce $msDll." }
  if ($LASTEXITCODE -ne 0) {
    Write-Host "mod_settings post-build step failed (LASTEXITCODE=$LASTEXITCODE) but $msDll exists; continuing."
    $LASTEXITCODE = 0
  }
} else {
  Write-Host "mod_settings.dll is up to date; skipping mod_settings build."
}

# Always re-pack the mod_settings packed.reds bundle. The cmake REDSCRIPT
# target is supposed to handle this automatically, but its incremental-
# build heuristic misses redscript edits in some cases (and the manual
# bash invocation of PackFiles.cmake mangles `//` to `/` via msys path
# conversion, producing malformed comment headers that fail compilation
# in-game). Running PackFiles via PowerShell with an explicitly quoted
# COMMENT_SLUG sidesteps both issues.
$msPackedReds = (Resolve-Path "vendor\mod_settings\build").ProviderPath + "\staging\packed.reds"
$msPackScript = (Resolve-Path "vendor\mod_settings\deps\cpcmake\scripts\PackFiles.cmake" -ErrorAction SilentlyContinue)
$msPackHeader = (Resolve-Path "vendor\mod_settings\deps\cpcmake\files\Header.txt" -ErrorAction SilentlyContinue)
$msPackSearch = (Resolve-Path "vendor\mod_settings\src\redscript" -ErrorAction SilentlyContinue)
if ($msPackScript -and $msPackHeader -and $msPackSearch) {
  Write-Host "Packing mod_settings redscript bundle..."
  $packedDir = Split-Path $msPackedReds -Parent
  if (-not (Test-Path $packedDir)) { New-Item -ItemType Directory -Force -Path $packedDir | Out-Null }
  & $cmake "-DCOMMENT_SLUG=//" "-DHEADER_FILE=$($msPackHeader.ProviderPath)" "-DPACKED_FILE=$msPackedReds" "-DGLOB_EXT=reds" "-DSEARCH_FOLDER=$($msPackSearch.ProviderPath)" -P $msPackScript.ProviderPath | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "mod_settings packed.reds generation failed." }
  if (-not (Test-Path $msPackedReds)) { throw "mod_settings packed.reds expected at $msPackedReds but missing." }
}

# Note: ModSettings.archive (the inkwidget pack output) is NOT regenerated
# from this script. The Wolvenkit CLI's pack and convert paths produce
# binaries that aren't equivalent to Wolvenkit GUI's "Pack Project" output
# and break the in-game .inkmenu integration. Pack via the GUI; deploy.ps1
# reads the archive from src/wolvenkit/packed/archive/pc/mod/ directly.

# ---------- gwheel.dll -----------------------------------------------------

Write-Host "Configuring gwheel..."
& $cmake -S . -B $BuildDir -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

Write-Host "Building gwheel..."
& $cmake --build $BuildDir --config $Config
if ($LASTEXITCODE -ne 0) { throw "CMake build failed." }

$dll = Join-Path $BuildDir "gwheel\$Config\gwheel.dll"
if (Test-Path $dll) {
  Write-Host ""
  Write-Host "Built: $dll"
  Write-Host "Built: $msDll"
  Write-Host ""
  Write-Host "Run deploy.ps1 to package or push to a game install."
} else {
  throw "Build succeeded but gwheel.dll was not found at $dll"
}
