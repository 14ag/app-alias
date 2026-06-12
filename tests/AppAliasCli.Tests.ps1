param(
    [Parameter(Mandatory)]
    [string]$CliPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Invoke-Cli {
    param([string[]]$Arguments)

    $stdout = [System.IO.Path]::GetTempFileName()
    $stderr = [System.IO.Path]::GetTempFileName()
    try {
        $startInfo = @{
            FilePath = $CliPath
            NoNewWindow = $true
            Wait = $true
            PassThru = $true
            RedirectStandardOutput = $stdout
            RedirectStandardError = $stderr
        }
        if ($Arguments.Count -gt 0) {
            $startInfo.ArgumentList = $Arguments
        }
        $process = Start-Process @startInfo
        [pscustomobject]@{
            ExitCode = $process.ExitCode
            Stdout = Get-Content -LiteralPath $stdout -Raw -ErrorAction SilentlyContinue
            Stderr = Get-Content -LiteralPath $stderr -Raw -ErrorAction SilentlyContinue
        }
    }
    finally {
        Remove-Item -LiteralPath $stdout, $stderr -Force -ErrorAction SilentlyContinue
    }
}

$help = Invoke-Cli @("--help")
if ($help.ExitCode -ne 0) {
    throw "--help returned $($help.ExitCode)"
}

$usage = Invoke-Cli @()
if ($usage.ExitCode -ne 2) {
    throw "empty args returned $($usage.ExitCode), expected 2"
}

$conflict = Invoke-Cli @("remove", "--alias", "a.exe", "--package", "p")
if ($conflict.ExitCode -ne 2) {
    throw "remove conflict returned $($conflict.ExitCode), expected 2"
}

$missing = Invoke-Cli @("verify", "--alias", "definitely-missing-appalias-test.exe")
if ($missing.ExitCode -ne 3) {
    throw "missing verify returned $($missing.ExitCode), expected 3"
}

$list = Invoke-Cli @("list", "--json")
if ($list.ExitCode -ne 0) {
    throw "list --json returned $($list.ExitCode)"
}
$null = $list.Stdout | ConvertFrom-Json
if ($list.Stderr) {
    throw "list --json wrote stderr: $($list.Stderr)"
}

Write-Output "[PASS] AppAliasCli.Tests"
