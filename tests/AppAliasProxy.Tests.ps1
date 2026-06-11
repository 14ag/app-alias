param(
    [Parameter(Mandatory)]
    [string]$ProxyPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$root = Join-Path ([System.IO.Path]::GetTempPath()) "AppAlias Proxy Test"
$proxy = Join-Path $root "AppAlias.Proxy.exe"
$capture = Join-Path $root "capture.cmd"
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

    [pscustomobject]@{
        target = $capture
    } | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $config -Encoding UTF8

    & $proxy alpha "two words"
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 7) {
        throw "proxy returned $exitCode, expected 7"
    }

    $forwarded = (Get-Content -LiteralPath $argsFile -Raw).Trim()
    if ($forwarded -ne '"alpha" "two words"') {
        throw "forwarded args mismatch: $forwarded"
    }

    Write-Output "[PASS] AppAliasProxy.Tests"
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
