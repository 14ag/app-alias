Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Find-Tool {
    param([Parameter(Mandatory)][string]$Name)

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $sdkRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    if (Test-Path -LiteralPath $sdkRoot) {
        $match = Get-ChildItem -LiteralPath $sdkRoot -Recurse -Filter $Name -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\' } |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($match) {
            return $match.FullName
        }
    }

    return $null
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vsInstall = $null
if (Test-Path -LiteralPath $vswhere) {
    $vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}

$required = [ordered]@{
    cmake = (Get-Command cmake.exe -ErrorAction SilentlyContinue).Source
    cl = if ($vsInstall) { Join-Path $vsInstall "VC\Auxiliary\Build\vcvars64.bat" } else { $null }
    msbuild = if ($vsInstall) { Join-Path $vsInstall "MSBuild\Current\Bin\MSBuild.exe" } else { $null }
    makeappx = Find-Tool "makeappx.exe"
    signtool = Find-Tool "signtool.exe"
}

$missing = @()
foreach ($item in $required.GetEnumerator()) {
    if (-not $item.Value -or -not (Test-Path -LiteralPath $item.Value -ErrorAction SilentlyContinue)) {
        $missing += $item.Key
    }
}

[pscustomobject]@{
    Missing = $missing
    Tools = $required
} | ConvertTo-Json -Depth 4

if ($missing.Count -gt 0) {
    exit 1
}
