param(
    [switch]$Machine,
    [switch]$Force,
    [switch]$ExportPfx
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$CertDir = Join-Path $env:LOCALAPPDATA "AppAliasGenerator\Cert"
New-Item -ItemType Directory -Force -Path $CertDir | Out-Null

$pfxPath = Join-Path $CertDir "AppAliasGenerator.pfx"
$cerPath = Join-Path $CertDir "AppAliasGenerator.cer"

if (((Test-Path -LiteralPath $pfxPath) -or (Test-Path -LiteralPath $cerPath)) -and (-not $Force)) {
    throw "Certificate files already exist. Re-run with -Force to replace them."
}

if ($Force) {
    Remove-Item -LiteralPath $pfxPath -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $cerPath -Force -ErrorAction SilentlyContinue
}

$cert = New-SelfSignedCertificate `
    -Type Custom `
    -Subject "CN=AppAliasGenerator" `
    -KeyUsage DigitalSignature `
    -FriendlyName "AppAliasGenerator" `
    -CertStoreLocation "Cert:\CurrentUser\My" `
    -NotAfter (Get-Date).AddYears(10) `
    -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3")

if ($ExportPfx) {
    if (-not $env:APPALIAS_PFX_PASSWORD) {
        throw "APPALIAS_PFX_PASSWORD must be set when -ExportPfx is used."
    }
    $password = ConvertTo-SecureString $env:APPALIAS_PFX_PASSWORD -Force -AsPlainText
    Export-PfxCertificate -Cert $cert -FilePath $pfxPath -Password $password | Out-Null
}
Export-Certificate -Cert $cert -FilePath $cerPath | Out-Null
Import-Certificate -FilePath $cerPath -CertStoreLocation "Cert:\CurrentUser\TrustedPeople" | Out-Null
Write-Warning "Importing test certificate into CurrentUser Root for MSIX trust."
Import-Certificate -FilePath $cerPath -CertStoreLocation "Cert:\CurrentUser\Root" | Out-Null

$machineTrusted = $false
if ($Machine) {
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $isAdmin) {
        throw "Machine certificate trust requires elevated Windows PowerShell."
    }

    Import-Certificate -FilePath $cerPath -CertStoreLocation "Cert:\LocalMachine\TrustedPeople" | Out-Null
    Import-Certificate -FilePath $cerPath -CertStoreLocation "Cert:\LocalMachine\Root" | Out-Null
    $machineTrusted = $true
}

[pscustomobject]@{
    PfxPath = $pfxPath
    CerPath = $cerPath
    Subject = $cert.Subject
    Thumbprint = $cert.Thumbprint
    CurrentUserTrusted = $true
    LocalMachineTrusted = $machineTrusted
} | ConvertTo-Json -Depth 3
