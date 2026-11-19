# One-click setup for templeos-recomp: TempleOS V5.03, statically recompiled,
# in a window on this PC. Run it by double-clicking Setup.cmd in the repo
# folder; the README's "By hand" is the same thing command for command.
#
# It checks the tools, takes your TempleOS.ISO (TempleOS is public domain; this
# repository contains none of it), lifts the kernel, builds, then runs the
# capture loop - hidden, muted runs that lift the compiler and the code it
# makes while TempleOS boots - and leaves "TempleOS.cmd" here. Each step is
# skipped when its output already exists (-Force redoes them). Everything goes
# to setup.log. -Yes installs what is missing without asking; -Iso is the path
# to TempleOS.ISO, asked for when not given.
param([switch]$Force, [switch]$Yes, [string]$Iso = "")

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Parent = Split-Path -Parent $Root
$Toolkit = if ($env:PCRECOMP) { $env:PCRECOMP } else { Join-Path $Parent 'pcrecomp-temple' }
$Log = Join-Path $Root 'setup.log'
$Work = Join-Path $Root 'work'
Set-Location $Root
function Log($t) { try { Add-Content -Path $Log -Value $t -Encoding UTF8 -ErrorAction Stop } catch {} }
function Say($t, $c = 'Gray') { Write-Host $t -ForegroundColor $c; Log $t }
function Pause-End { if (-not $env:RECOMP_LAUNCHER) { Read-Host "Press Enter to close" | Out-Null } }
function Step($n, $t) { Write-Host ""; Say "[$n/6] $t" 'Cyan' }
function Fail($t) {
  Say ""; Say "Setup stopped: $t" 'Red'
  Say "The details are in $Log. Fix that and run Setup.cmd again; finished steps are skipped." 'Yellow'
  Pause-End; exit 1
}
function Ask($q) { if ($Yes) { Say "$q yes"; return $true }; $a = Read-Host "$q [Y/n]"; return -not ($a -match '^[nN]') }
function Exec([string[]]$cmd) {
  $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  $rest = @($cmd | Select-Object -Skip 1)
  & $cmd[0] @rest 2>&1 | ForEach-Object { Log "$_" }
  $code = $LASTEXITCODE
  $ErrorActionPreference = $old
  return $code
}
function Run($what, [string[]]$cmd) { Say "  $what..."; $code = Exec $cmd; if ($code -ne 0) { Fail "$what failed (exit code $code)." } }
function Done($p) { return (-not $Force) -and (Test-Path -LiteralPath $p) }

Log "==== setup $(Get-Date -Format s)"
Clear-Host
Say "TempleOS V5.03 - static recompilation setup" 'White'
Say "TempleOS was written by Terry A. Davis (1969-2018) and is in the public domain."
Say "You need TempleOS.ISO (V5.03). Lifting and building take a while, once."

# ---------------------------------------------------------------- tools
Step 1 "Checking the tools"
if (-not (Get-Command py -ErrorAction SilentlyContinue)) {
  if (Ask "Python 3 is missing. Install it with winget?") { Run "Installing Python" @('winget', 'install', '-e', '--id', 'Python.Python.3.12', '--accept-package-agreements', '--accept-source-agreements') }
  else { Fail "Python 3.10 or newer is needed." }
}
if ((Exec @('py', '-3', '-c', 'import capstone')) -ne 0) { Run "Installing capstone" @('py', '-3', '-m', 'pip', 'install', '--user', 'capstone') }
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
  if (Ask "CMake is missing. Install it with winget?") { Run "Installing CMake" @('winget', 'install', '-e', '--id', 'Kitware.CMake', '--accept-package-agreements', '--accept-source-agreements') }
  else { Fail "CMake is needed." }
  $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere) -or -not (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)) {
  Fail "Visual Studio 2022 (or Build Tools) with 'Desktop development with C++' is needed."
}
if (-not (Test-Path (Join-Path $Toolkit 'tools\lift\lift64_sys.py'))) {
  Fail "pcrecomp's 'temple' branch was not found at $Toolkit (set PCRECOMP to its folder)."
}
# SDL2: vcpkg when present, otherwise SDL's own development package.
$Sdl = Join-Path $Work 'SDL2'
$Toolchain = if ($env:VCPKG_ROOT) { Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake' } elseif (Test-Path 'C:\vcpkg') { 'C:\vcpkg\scripts\buildsystems\vcpkg.cmake' } else { '' }
$CMakeArgs = @()
if ($Toolchain -and (Test-Path $Toolchain)) {
  $CMakeArgs = @("-DCMAKE_TOOLCHAIN_FILE=$Toolchain")
  Run "Making sure SDL2 is installed (vcpkg)" @((Join-Path (Split-Path (Split-Path (Split-Path $Toolchain))) 'vcpkg.exe'), 'install', 'sdl2:x64-windows')
} else {
  if (-not (Done $Sdl)) {
    New-Item -ItemType Directory -Force $Work | Out-Null
    $zip = Join-Path $Work 'SDL2.zip'
    Say "  Downloading SDL2 2.30.9 (development package)..."
    Invoke-WebRequest 'https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-VC.zip' -OutFile $zip
    Expand-Archive $zip -DestinationPath $Work -Force
    Move-Item (Join-Path $Work 'SDL2-2.30.9') $Sdl
  }
  $CMakeArgs = @("-DSDL2_DIR=$Sdl\cmake")
}

# ---------------------------------------------------------------- ISO
Step 2 "TempleOS.ISO"
$IsoDst = Join-Path $Work 'TempleOS.ISO'
if (-not (Done $IsoDst)) {
  while (-not $Iso -or -not (Test-Path -LiteralPath $Iso)) { $Iso = (Read-Host "Path to TempleOS.ISO").Trim('"') }
  New-Item -ItemType Directory -Force $Work | Out-Null
  Copy-Item -LiteralPath $Iso $IsoDst
}
Say "  $IsoDst"

# ---------------------------------------------------------------- lift + build
Step 3 "Lifting the kernel"
$env:PCRECOMP = $Toolkit
Run "tools/gen.py" @('py', '-3', 'tools\gen.py', $IsoDst, $Toolkit)
Step 4 "Configuring the build"
Run "cmake" (@('cmake', '-S', '.', '-B', 'build', '-G', 'Visual Studio 17 2022', '-A', 'x64', "-DPCRECOMP_DIR=$Toolkit") + $CMakeArgs)
Step 5 "Capturing and lifting the code TempleOS makes while it boots (hidden, muted runs)"
Say "  This repeats lift -> build -> boot until the boot runs through; it takes a while the first time."
Run "tools/capture.py" @('py', '-3', 'tools\capture.py', $IsoDst)

# ---------------------------------------------------------------- launcher
Step 6 "Launcher"
$Exe = Join-Path $Root 'build\Release\templeos.exe'
if (-not (Test-Path $Exe)) { Fail "the build did not produce $Exe." }
Set-Content -Path (Join-Path $Root 'TempleOS.cmd') -Encoding ASCII -Value "@echo off`r`n`"%~dp0build\Release\templeos.exe`" `"%~dp0work\TempleOS.ISO`" %*`r`n"
Say "Done. Start TempleOS with TempleOS.cmd." 'Green'
Pause-End
