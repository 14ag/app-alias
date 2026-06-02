@echo off
setlocal
set "SCRIPT_DIR=%~dp0"

:: Auto-bootstrap: ensure the script directory is on User PATH
powershell -NoProfile -ExecutionPolicy Bypass -Command "Import-Module '%SCRIPT_DIR%AliasManager.psm1'; $dir = [System.IO.Path]::GetFullPath('%SCRIPT_DIR%'.TrimEnd('\')); Add-ToPathVariable -Directory $dir -Scope User" >nul 2>&1

:: Process arguments
set "ARGS=%*"
if not defined ARGS (
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Import-Module '%SCRIPT_DIR%AliasManager.psm1'; Get-AppAlias"
    exit /b
)

:: Escape quotes for PowerShell
set "ARGS=%ARGS:"=\"%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "Import-Module '%SCRIPT_DIR%AliasManager.psm1'; & { $argsList = @($args); $first = $argsList[0]; $remaining = if ($argsList.Count -gt 1) { $argsList[1..($argsList.Count-1)] } else { @() }; if ($first -in 'get', 'list') { if ($remaining.Count -gt 0) { Get-AppAlias @remaining } else { Get-AppAlias } } elseif ($first -in 'remove', 'delete', 'uninstall') { if ($remaining.Count -gt 0) { Remove-AppAlias @remaining } else { Remove-AppAlias } } elseif ($first -in 'new', 'add', 'install') { if ($remaining.Count -gt 0) { New-AppAlias @remaining } else { New-AppAlias } } elseif ($first -in '-?', '/?', 'help', '--help', '-h') { Write-Host 'Usage: alias [add|remove|get] [parameters]' -ForegroundColor Yellow; Write-Host '  alias add -ExePath <path> [-Alias <name>] [-Scope User|Machine]' -ForegroundColor Gray; Write-Host '  alias remove -ProductCode <name> [-Scope User|Machine]' -ForegroundColor Gray; Write-Host '  alias get' -ForegroundColor Gray } else { New-AppAlias @argsList } }" %ARGS%
