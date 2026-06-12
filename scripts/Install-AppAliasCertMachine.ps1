Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptPath = Join-Path $PSScriptRoot "New-AppAliasCert.ps1"
$Helper = Join-Path (Split-Path -Parent $PSScriptRoot) ".vscode\get_admin.bat"
if (-not (Test-Path -LiteralPath $Helper)) {
    throw ".vscode\get_admin.bat not found"
}

$ArgsLine = "/c `"`"$Helper`" powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"`"$ScriptPath`"`" -Machine`""
$process = Start-Process -FilePath "cmd.exe" -ArgumentList $ArgsLine -Wait -PassThru
if ($process.ExitCode -ne 0) {
    throw "machine certificate install failed with exit code $($process.ExitCode)"
}
