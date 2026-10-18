<#
.SYNOPSIS
  One-command Release build of Vagrant Story Recompiled.

.DESCRIPTION
  Configures and builds the Release configuration (VS 2022 MSVC + Ninja, PGXP
  hook flavor, static overlays from generated/ when present) in build-release\,
  then stages everything the game needs to run into one folder
  (dist\release\ by default): exe, game.toml, game_options.toml, assets, bios,
  bundled mods, overlay_toolchain and third-party licenses.

  It never touches build\ (the folder you play from), its save states,
  memory cards, settings or mods\state.toml.

.EXAMPLE
  pwsh -File build_release.ps1
  pwsh -File build_release.ps1 -Clean -OutDir C:\vs_release
  pwsh -File build_release.ps1 -DiscCode

  Run scripts/apply_patches.ps1 first, and generate generated/ from your disc
  (README, step 3) unless you pass -DiscCode.
#>
param(
    # Delete build-release\ first for a from-scratch build.
    [switch]$Clean,
    # Where the runnable game is staged (wiped and recreated each run).
    [string]$OutDir = "",
    # Build tree (relative to the repo root).
    [string]$BuildDir = "build-release",
    # Parallel compile jobs (0 = Ninja default, one per core). Lower it if the
    # PC runs out of memory on the large generated C files.
    [int]$Jobs = 0,
    # Build the variant with no game code inside (-DPSXRECOMP_DISC_CODE=ON); it
    # compiles the game from the disc at runtime, so generated/ is not needed.
    [switch]$DiscCode,
    # Build even if the framework is not on its patched local branch.
    [switch]$SkipBranchCheck
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $Root "dist\release" }
if (-not [IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $Root $BuildDir }
$ExeName = "VagrantStory_Recompiled.exe"

function Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Fail($msg) { Write-Host "error: $msg" -ForegroundColor Red; exit 1 }

# --- Sanity checks -----------------------------------------------------------
# The build needs the framework on its patched local branch (the bare upstream
# pin fails with C11 atomics errors under MSVC).
if (-not $SkipBranchCheck) {
    $fwBranch = (git -C (Join-Path $Root "psxrecomp") branch --show-current).Trim()
    if ($fwBranch -ne "vagrant-story-local") {
        Fail "psxrecomp is on '$fwBranch', expected 'vagrant-story-local': run scripts/apply_patches.ps1 (or use -SkipBranchCheck)"
    }
}
if (-not $DiscCode -and -not (Test-Path (Join-Path $Root "generated\SLUS_010.40_dispatch.c"))) {
    Fail "generated\ is missing: generate the game C from your disc first (README, step 3), or pass -DiscCode"
}
$discCodeFlag = if ($DiscCode) { "ON" } else { "OFF" }

# --- MSVC environment --------------------------------------------------------
# Pin VS 2022 (version 17.x): VS 18's STL breaks the build.
if (-not $env:VCToolsInstallDir) {
    Step "Loading the VS 2022 x64 build environment"
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { Fail "vswhere.exe not found; is Visual Studio 2022 installed?" }
    $vs = & $vswhere -version "[17.0,18.0)" -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
    if (-not $vs) { Fail "Visual Studio 2022 with the C++ tools was not found" }
    $vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
    $envDump = cmd /c "`"$vcvars`" >nul && set"
    if ($LASTEXITCODE -ne 0) { Fail "vcvars64.bat failed" }
    foreach ($line in $envDump) {
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2]) }
    }
}
foreach ($tool in "cmake", "ninja", "cl") {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { Fail "$tool not on PATH after loading vcvars" }
}

# --- Configure + build -------------------------------------------------------
if ($Clean -and (Test-Path $BuildDir)) {
    Step "Removing $BuildDir"
    Remove-Item -Recurse -Force $BuildDir
}

Step "Configuring $BuildDir (Release)"
cmake -S $Root -B $BuildDir -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DPSX_DEBUG_TOOLS=OFF `
    -DVS_PGXP_HOOKS=ON `
    -DPSXRECOMP_DISC_CODE=$discCodeFlag `
    -DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON
if ($LASTEXITCODE -ne 0) { Fail "CMake configure failed" }

Step "Building"
$jobArgs = @()
if ($Jobs -gt 0) { $jobArgs = @("--parallel", $Jobs) }
cmake --build $BuildDir --config Release @jobArgs
if ($LASTEXITCODE -ne 0) { Fail "build failed" }

$exe = Join-Path $BuildDir $ExeName
if (-not (Test-Path $exe)) { Fail "$exe was not produced" }

# --- Stage the runnable game -------------------------------------------------
Step "Staging $OutDir"
if (Test-Path $OutDir) { Remove-Item -Recurse -Force $OutDir }
New-Item -ItemType Directory -Force $OutDir | Out-Null

foreach ($f in $ExeName, "game.toml", "game_options.toml", "psx_game_version.txt") {
    $src = Join-Path $BuildDir $f
    if (Test-Path $src) { Copy-Item $src $OutDir }
}
foreach ($d in "assets", "bios") {
    $src = Join-Path $BuildDir $d
    if (-not (Test-Path $src)) { Fail "$src missing from the build output" }
    Copy-Item -Recurse $src $OutDir
}

# Mods: the shipped catalog only, never the player's enable state.
$modsOut = New-Item -ItemType Directory -Force (Join-Path $OutDir "mods")
foreach ($m in "README.md", "bundled", "texture-packs") {
    $src = Join-Path $BuildDir "mods\$m"
    if (Test-Path $src) { Copy-Item -Recurse $src $modsOut }
}

# Overlay toolchain (TinyCC/Python used to compile overlays not baked into the
# exe). The build does not create it; if you staged one into build\ (README),
# take it from there when the build tree has none.
$tc = Join-Path $BuildDir "overlay_toolchain"
if (-not (Test-Path (Join-Path $tc "tcc"))) { $tc = Join-Path $Root "build\overlay_toolchain" }
if (Test-Path $tc) {
    Copy-Item -Recurse $tc (Join-Path $OutDir "overlay_toolchain")
    Get-ChildItem (Join-Path $OutDir "overlay_toolchain") -Recurse -Directory -Filter "__pycache__" |
        Remove-Item -Recurse -Force
} else {
    Write-Warning "no overlay_toolchain found; overlays outside the exe will run interpreted"
}

# Third-party notices.
$lic = New-Item -ItemType Directory -Force (Join-Path $OutDir "licenses")
Copy-Item (Join-Path $Root "psxrecomp\runtime\licenses\*") $lic
Copy-Item (Join-Path $Root "psxrecomp\THIRD_PARTY_ATTRIBUTION.md") $lic
Copy-Item (Join-Path $Root "psxrecomp\LICENSE") (Join-Path $lic "psxrecomp-LICENSE")
Copy-Item (Join-Path $Root "recomp-ui\LICENSE") (Join-Path $lic "recomp-ui-LICENSE")

$version = (Get-Content (Join-Path $Root "VERSION") -Raw).Trim()
Write-Host ""
Write-Host "Vagrant Story Recompiled $version (Release) is ready:" -ForegroundColor Green
Write-Host "  $(Join-Path $OutDir $ExeName)"
