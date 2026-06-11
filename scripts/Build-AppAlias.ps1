Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $VsWhere)) {
    throw "vswhere.exe not found"
}

$VsInstall = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $VsInstall) {
    throw "Visual Studio C++ Build Tools not found"
}

$DevCmd = Join-Path $VsInstall "Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path -LiteralPath $DevCmd)) {
    throw "VsDevCmd.bat not found"
}

$BuildDir = Join-Path $Root "build"
$Command = "call `"$DevCmd`" -arch=x64 -host_arch=x64 && cmake -S `"$Root`" -B `"$BuildDir`" -G `"Visual Studio 17 2022`" -A x64 && cmake --build `"$BuildDir`" --config Release && ctest --test-dir `"$BuildDir`" -C Release --output-on-failure"
cmd.exe /d /s /c $Command
exit $LASTEXITCODE
