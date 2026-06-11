Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptPath = Join-Path $PSScriptRoot "New-AppAliasCert.ps1"
$ArgsLine = "-NoProfile -ExecutionPolicy Bypass -File `"$ScriptPath`" -Machine"
Start-Process -FilePath "powershell.exe" -ArgumentList $ArgsLine -Verb RunAs -Wait
