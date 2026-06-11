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

function Find-Cl {
    param([string]$VsInstall)

    if (-not $VsInstall) {
        return $null
    }

    $toolsRoot = Join-Path $VsInstall "VC\Tools\MSVC"
    if (-not (Test-Path -LiteralPath $toolsRoot)) {
        return $null
    }

    $match = Get-ChildItem -LiteralPath $toolsRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName "bin\Hostx64\x64\cl.exe" } |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1

    return $match
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vsInstall = $null
if (Test-Path -LiteralPath $vswhere) {
    $vsInstall = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
}

$required = [ordered]@{
    cmake = (Get-Command cmake.exe -ErrorAction SilentlyContinue).Source
    cl = Find-Cl $vsInstall
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
