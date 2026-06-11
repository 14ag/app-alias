Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $VsWhere)) {
    throw "vswhere.exe not found"
}

$VsInstall = (& $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
if (-not $VsInstall) {
    throw "Visual Studio C++ Build Tools not found"
}

$VsVersion = (& $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion).Trim()
$VsMajor = [int]($VsVersion.Split(".")[0])
$Generator = switch ($VsMajor) {
    17 { "Visual Studio 17 2022" }
    16 { "Visual Studio 16 2019" }
    default { throw "Unsupported Visual Studio major version: $VsMajor" }
}

$DevCmd = Join-Path $VsInstall "Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path -LiteralPath $DevCmd)) {
    throw "VsDevCmd.bat not found"
}

$BuildDir = Join-Path $Root "build"
$TempCmd = Join-Path ([System.IO.Path]::GetTempPath()) ("appalias-build-{0}.cmd" -f [guid]::NewGuid())
$Lines = @(
    "@echo off",
    "call `"$DevCmd`" -arch=x64 -host_arch=x64",
    "if errorlevel 1 exit /b %errorlevel%",
    "cmake -S `"$Root`" -B `"$BuildDir`" -G `"$Generator`" -A x64",
    "if errorlevel 1 exit /b %errorlevel%",
    "cmake --build `"$BuildDir`" --config Release",
    "if errorlevel 1 exit /b %errorlevel%",
    "ctest --test-dir `"$BuildDir`" -C Release --output-on-failure"
)

try {
    Set-Content -LiteralPath $TempCmd -Value $Lines -Encoding ASCII
    cmd.exe /d /c "`"$TempCmd`""
    exit $LASTEXITCODE
}
finally {
    Remove-Item -LiteralPath $TempCmd -Force -ErrorAction SilentlyContinue
}
