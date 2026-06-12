param(
    [Parameter(Mandatory)]
    [string]$ProxyPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-AliasConfig {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Target
    )

    $json = [pscustomobject]@{ target = $Target } | ConvertTo-Json -Depth 3
    [System.IO.File]::WriteAllText($Path, $json, [System.Text.UTF8Encoding]::new($false))
}

$root = Join-Path ([System.IO.Path]::GetTempPath()) "AppAlias Proxy Test"
$proxy = Join-Path $root "AppAlias.Proxy.exe"
$capture = Join-Path $root "capture.cmd"
$psCapture = Join-Path $root "capture.ps1"
$argsFile = Join-Path $root "args.txt"
$config = Join-Path $root "alias.json"

Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $root | Out-Null

try {
    Copy-Item -LiteralPath $ProxyPath -Destination $proxy -Force

    @(
        "@echo off",
        "echo %*>`"%~dp0args.txt`"",
        "exit /b 7"
    ) | Set-Content -LiteralPath $capture -Encoding ASCII

    Write-AliasConfig -Path $config -Target $capture

    & $proxy alpha "two words"
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 7) {
        throw "proxy returned $exitCode, expected 7"
    }

    $forwarded = (Get-Content -LiteralPath $argsFile -Raw).Trim()
    if ($forwarded -ne '"alpha" "two words"') {
        throw "forwarded args mismatch: $forwarded"
    }

    @(
        "param([Parameter(ValueFromRemainingArguments=`$true)][string[]]`$Forwarded)",
        "[System.IO.File]::WriteAllText(`"$argsFile`", (`$Forwarded -join '|'), [System.Text.UTF8Encoding]::new(`$false))",
        "exit 9"
    ) | Set-Content -LiteralPath $psCapture -Encoding ASCII

    Write-AliasConfig -Path $config -Target $psCapture
    & $proxy alpha "two words" ""
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 9) {
        throw "ps1 proxy returned $exitCode, expected 9"
    }

    $forwarded = Get-Content -LiteralPath $argsFile -Raw
    if ($forwarded -ne 'alpha|two words') {
        throw "ps1 forwarded args mismatch: $forwarded"
    }

    Write-AliasConfig -Path $config -Target "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    & $proxy -NoProfile -Command "exit 6"
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 6) {
        throw "exe proxy returned $exitCode, expected 6"
    }

    Write-AliasConfig -Path $config -Target (Join-Path $root "missing.exe")
    & $proxy
    if ($LASTEXITCODE -eq 0) {
        throw "missing target should fail"
    }

    Write-Output "[PASS] AppAliasProxy.Tests"
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
