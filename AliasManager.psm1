#Requires -Version 5.1
<#
.SYNOPSIS
    Programmatic app alias manager for Windows.

.DESCRIPTION
    Implements winget-style portable aliases plus external-target aliases for
    installed applications that must keep their original resource directory.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-PortableLinksLocation {
    param([ValidateSet('User','Machine')] [string]$Scope = 'User')

    if ($Scope -eq 'Machine') {
        return [System.IO.Path]::Combine($env:ProgramFiles, 'WinGet', 'Links')
    }

    return [System.IO.Path]::Combine($env:LOCALAPPDATA, 'Microsoft', 'WinGet', 'Links')
}

function Get-PortableInstallRoot {
    param(
        [ValidateSet('User','Machine')] [string]$Scope = 'User',
        [ValidateSet('x86','x64','arm64','neutral')] [string]$Arch = 'x64'
    )

    if ($Scope -eq 'Machine') {
        $base = if ($Arch -eq 'x86') { "${env:ProgramFiles(x86)}" } else { $env:ProgramFiles }
        return [System.IO.Path]::Combine($base, 'WinGet', 'Packages')
    }

    return [System.IO.Path]::Combine($env:LOCALAPPDATA, 'Microsoft', 'WinGet', 'Packages')
}

function Resolve-AliasName {
    param(
        [string]$ExePath,
        [string]$Alias = '',
        [string]$Rename = ''
    )

    $name = [System.IO.Path]::GetFileNameWithoutExtension($ExePath)
    if ($Alias) { $name = [System.IO.Path]::GetFileNameWithoutExtension($Alias) }
    if ($Rename) { $name = [System.IO.Path]::GetFileNameWithoutExtension($Rename) }
    return "$name.exe"
}

function Get-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Assert-ScopeWritable {
    param([ValidateSet('User','Machine')] [string]$Scope)

    if ($Scope -eq 'Machine' -and -not (Get-IsAdministrator)) {
        throw 'Machine scope requires elevated PowerShell.'
    }
}

function Get-ARPKeyPath {
    param(
        [string]$ProductCode,
        [ValidateSet('User','Machine')] [string]$Scope = 'User'
    )

    $hive = if ($Scope -eq 'Machine') { 'HKLM' } else { 'HKCU' }
    return "$hive`:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Portable_$ProductCode"
}

function Get-AppPathsKeyPath {
    param(
        [string]$AliasName,
        [ValidateSet('User','Machine')] [string]$Scope = 'User'
    )

    $hive = if ($Scope -eq 'Machine') { 'HKLM' } else { 'HKCU' }
    return "$hive`:\Software\Microsoft\Windows\CurrentVersion\App Paths\$AliasName"
}

function Get-PropertyValue {
    param(
        [object]$Properties,
        [string[]]$Names,
        $Default = ''
    )

    if ($null -eq $Properties) { return $Default }
    foreach ($name in $Names) {
        if ($Properties.PSObject.Properties.Name -contains $name) {
            $value = $Properties.$name
            if ($null -ne $value -and "$value" -ne '') { return $value }
        }
    }
    return $Default
}

function Get-ARPEntry {
    param(
        [string]$ProductCode,
        [ValidateSet('User','Machine')] [string]$Scope = 'User'
    )

    $path = Get-ARPKeyPath -ProductCode $ProductCode -Scope $Scope
    if (-not (Test-Path $path)) { return $null }

    $props = Get-ItemProperty -Path $path
    $aliasName = [System.IO.Path]::GetFileName((Get-PropertyValue $props @('SymlinkFullPath','PortableSymlinkFullPath')))
    $target = Get-PropertyValue $props @('TargetFullPath','PortableTargetFullPath')
    $launcher = Get-PropertyValue $props @('LauncherFullPath')
    $launchCommand = Get-PropertyValue $props @('LauncherCommandFullPath')
    $mode = Get-PropertyValue $props @('AliasMode') 'Portable'

    [pscustomobject]@{
        ProductCode = $ProductCode
        DisplayName = Get-PropertyValue $props @('DisplayName')
        DisplayVersion = Get-PropertyValue $props @('DisplayVersion')
        Publisher = Get-PropertyValue $props @('Publisher')
        Alias = $aliasName
        Target = $target
        Launcher = $launcher
        LaunchCommand = $launchCommand
        InstallLocation = Get-PropertyValue $props @('InstallLocation')
        Symlink = Get-PropertyValue $props @('SymlinkFullPath','PortableSymlinkFullPath')
        SHA256 = Get-PropertyValue $props @('SHA256')
        Mode = $mode
        InstallDirectoryCreated = [bool](Get-PropertyValue $props @('InstallDirectoryCreated') 0)
        InstallDirectoryAddedToPath = [bool](Get-PropertyValue $props @('InstallDirectoryAddedToPath') 0)
        Scope = $Scope
        RegistryPath = $path
    }
}

function Set-RegistryValue {
    param(
        [string]$Path,
        [string]$Name,
        $Value,
        [ValidateSet('String','DWord')] [string]$Type = 'String'
    )

    if ($null -eq $Value) { return }
    if ($Type -eq 'String' -and "$Value" -eq '') { return }

    $propertyType = if ($Type -eq 'DWord') { 'DWord' } else { 'String' }
    New-ItemProperty -Path $Path -Name $Name -Value $Value -PropertyType $propertyType -Force | Out-Null
}

function Get-FileSHA256 {
    param([Parameter(Mandatory)] [string]$Path)

    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try {
            $bytes = $sha.ComputeHash($stream)
            return (($bytes | ForEach-Object { $_.ToString('x2') }) -join '').ToUpperInvariant()
        } finally {
            $sha.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

function Register-ARPEntry {
    param(
        [string]$ProductCode,
        [string]$DisplayName,
        [string]$DisplayVersion = '',
        [string]$Publisher = '',
        [string]$InstallLocation,
        [string]$SymlinkFullPath,
        [string]$TargetFullPath,
        [string]$LauncherFullPath = '',
        [string]$LauncherCommandFullPath = '',
        [string]$SHA256 = '',
        [string]$AliasMode = 'Portable',
        [bool]$InstallDirectoryCreated = $false,
        [bool]$InstallDirectoryAddedToPath = $false,
        [string]$WinGetPackageId = '',
        [string]$WinGetSourceId = '',
        [ValidateSet('User','Machine')] [string]$Scope = 'User'
    )

    $keyPath = Get-ARPKeyPath -ProductCode $ProductCode -Scope $Scope
    if (-not (Test-Path $keyPath)) {
        New-Item -Path $keyPath -Force | Out-Null
    }

    $display = if ($DisplayName) { $DisplayName } else { $ProductCode }
    $date = Get-Date -Format 'yyyyMMdd'

    Set-RegistryValue -Path $keyPath -Name DisplayName -Value $display
    Set-RegistryValue -Path $keyPath -Name DisplayVersion -Value $DisplayVersion
    Set-RegistryValue -Path $keyPath -Name Publisher -Value $Publisher
    Set-RegistryValue -Path $keyPath -Name InstallDate -Value $date
    Set-RegistryValue -Path $keyPath -Name UninstallString -Value "powershell -NoProfile -Command `"Import-Module '$PSScriptRoot\AliasManager.psm1'; Remove-AppAlias -ProductCode '$ProductCode' -Scope $Scope`""
    Set-RegistryValue -Path $keyPath -Name WinGetInstallerType -Value 'Portable'
    Set-RegistryValue -Path $keyPath -Name InstallLocation -Value $InstallLocation
    Set-RegistryValue -Path $keyPath -Name TargetFullPath -Value $TargetFullPath
    Set-RegistryValue -Path $keyPath -Name SymlinkFullPath -Value $SymlinkFullPath
    Set-RegistryValue -Path $keyPath -Name LauncherFullPath -Value $LauncherFullPath
    Set-RegistryValue -Path $keyPath -Name LauncherCommandFullPath -Value $LauncherCommandFullPath
    Set-RegistryValue -Path $keyPath -Name SHA256 -Value $SHA256
    Set-RegistryValue -Path $keyPath -Name AliasMode -Value $AliasMode
    Set-RegistryValue -Path $keyPath -Name WinGetPackageIdentifier -Value $WinGetPackageId
    Set-RegistryValue -Path $keyPath -Name WinGetSourceIdentifier -Value $WinGetSourceId
    Set-RegistryValue -Path $keyPath -Name InstallDirectoryCreated -Value ([int]$InstallDirectoryCreated) -Type DWord
    Set-RegistryValue -Path $keyPath -Name InstallDirectoryAddedToPath -Value ([int]$InstallDirectoryAddedToPath) -Type DWord
    Set-RegistryValue -Path $keyPath -Name NoModify -Value 1 -Type DWord
    Set-RegistryValue -Path $keyPath -Name NoRepair -Value 1 -Type DWord
}

function Remove-ARPEntry {
    param(
        [string]$ProductCode,
        [ValidateSet('User','Machine')] [string]$Scope = 'User'
    )

    $keyPath = Get-ARPKeyPath -ProductCode $ProductCode -Scope $Scope
    if (Test-Path $keyPath) {
        Remove-Item $keyPath -Recurse -Force
    }
}

function Get-EnvironmentPathKey {
    param([ValidateSet('User','Machine')] [string]$Scope)

    if ($Scope -eq 'Machine') {
        return [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager\Environment', $true)
    }

    return [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $true)
}

function Get-NormalizedPathEntry {
    param([string]$Path)

    $expanded = [Environment]::ExpandEnvironmentVariables($Path.Trim())
    try {
        $expanded = [System.IO.Path]::GetFullPath($expanded)
    } catch {
    }
    return $expanded.TrimEnd('\')
}

function Test-PathValueContains {
    param([string]$PathValue, [string]$Directory)

    $target = Get-NormalizedPathEntry $Directory
    foreach ($part in ($PathValue -split ';')) {
        if ($part -eq '') { continue }
        if ((Get-NormalizedPathEntry $part) -ieq $target) { return $true }
    }
    return $false
}

function Send-EnvironmentChange {
    if (-not ('WinAPI.User32' -as [type])) {
        Add-Type -Namespace WinAPI -Name User32 -MemberDefinition @'
[DllImport("user32.dll", SetLastError=true, CharSet=CharSet.Auto)]
public static extern IntPtr SendMessageTimeout(
    IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam,
    uint fuFlags, uint uTimeout, out UIntPtr lpdwResult);
'@
    }

    $result = [UIntPtr]::Zero
    [WinAPI.User32]::SendMessageTimeout(
        [IntPtr]0xFFFF, 0x001A, [UIntPtr]::Zero, 'Environment',
        0x0002, 5000, [ref]$result) | Out-Null
}

function Add-ToPathVariable {
    param(
        [string]$Directory,
        [ValidateSet('User','Machine')] [string]$Scope = 'User'
    )

    Assert-ScopeWritable -Scope $Scope
    $key = Get-EnvironmentPathKey -Scope $Scope
    try {
        $current = [string]$key.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        if (Test-PathValueContains -PathValue $current -Directory $Directory) { return $false }

        $parts = @($current -split ';' | Where-Object { $_ -ne '' })
        $newPath = ($parts + $Directory) -join ';'
        $key.SetValue('Path', $newPath, [Microsoft.Win32.RegistryValueKind]::ExpandString)
        Send-EnvironmentChange
        return $true
    } finally {
        $key.Close()
    }
}

function Remove-FromPathVariable {
    param(
        [string]$Directory,
        [ValidateSet('User','Machine')] [string]$Scope = 'User'
    )

    Assert-ScopeWritable -Scope $Scope
    $key = Get-EnvironmentPathKey -Scope $Scope
    try {
        $current = [string]$key.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        $target = Get-NormalizedPathEntry $Directory
        $parts = @()
        $removed = $false

        foreach ($part in ($current -split ';')) {
            if ($part -eq '') { continue }
            if ((Get-NormalizedPathEntry $part) -ieq $target) {
                $removed = $true
                continue
            }
            $parts += $part
        }

        if ($removed) {
            $key.SetValue('Path', ($parts -join ';'), [Microsoft.Win32.RegistryValueKind]::ExpandString)
            Send-EnvironmentChange
        }

        return $removed
    } finally {
        $key.Close()
    }
}

function New-Symlink {
    param(
        [string]$LinkPath,
        [string]$TargetPath
    )

    $parent = [System.IO.Path]::GetDirectoryName($LinkPath)
    if (-not (Test-Path $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }

    if (Test-Path $LinkPath -PathType Container) {
        throw "Alias path points to a directory: $LinkPath"
    }

    if (Test-Path $LinkPath) {
        Remove-Item $LinkPath -Force
    }

    $output = & cmd /c mklink "$LinkPath" "$TargetPath" 2>&1
    if ($LASTEXITCODE -eq 0 -and (Test-Path $LinkPath)) { return $true }

    Write-Verbose ($output | Out-String)
    return $false
}

function ConvertTo-CSharpStringLiteral {
    param([string]$Value)

    $escaped = $Value.Replace('\', '\\').Replace('"', '\"').Replace("`r", '\r').Replace("`n", '\n').Replace("`t", '\t')
    return '"' + $escaped + '"'
}

function Get-CSharpCompiler {
    $cmd = Get-Command csc.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $candidates = @(
        "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe",
        "$env:WINDIR\Microsoft.NET\Framework\v4.0.30319\csc.exe"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }

    throw 'csc.exe not found. Install .NET Framework compiler or use portable mode.'
}

function New-ExternalLauncher {
    param(
        [string]$LaunchPath,
        [string]$LauncherPath
    )

    $compiler = Get-CSharpCompiler
    $launcherDir = [System.IO.Path]::GetDirectoryName($LauncherPath)
    if (-not (Test-Path $launcherDir)) {
        New-Item -ItemType Directory -Path $launcherDir -Force | Out-Null
    }

    $targetLiteral = ConvertTo-CSharpStringLiteral $LaunchPath
    $sourcePath = [System.IO.Path]::ChangeExtension($LauncherPath, '.cs')
    $source = @"
using System;
using System.Diagnostics;
using System.IO;
using System.Text;

internal static class AppAliasLauncher
{
    private const string LaunchPath = $targetLiteral;

    [STAThread]
    private static int Main(string[] args)
    {
        ProcessStartInfo psi = new ProcessStartInfo();
        psi.UseShellExecute = false;
        psi.WorkingDirectory = Path.GetDirectoryName(LaunchPath);

        string extension = Path.GetExtension(LaunchPath);
        if (StringComparer.OrdinalIgnoreCase.Equals(extension, ".cmd") ||
            StringComparer.OrdinalIgnoreCase.Equals(extension, ".bat"))
        {
            string comspec = Environment.GetEnvironmentVariable("COMSPEC");
            psi.FileName = String.IsNullOrEmpty(comspec) ? "cmd.exe" : comspec;
            string joined = JoinArguments(args);
            psi.Arguments = "/d /c " + QuoteArgument(LaunchPath) + (joined.Length == 0 ? "" : " " + joined);
        }
        else
        {
            psi.FileName = LaunchPath;
            psi.Arguments = JoinArguments(args);
        }

        using (Process process = Process.Start(psi))
        {
            process.WaitForExit();
            return process.ExitCode;
        }
    }

    private static string JoinArguments(string[] args)
    {
        StringBuilder result = new StringBuilder();
        for (int i = 0; i < args.Length; ++i)
        {
            if (i > 0)
            {
                result.Append(' ');
            }
            result.Append(QuoteArgument(args[i]));
        }
        return result.ToString();
    }

    private static string QuoteArgument(string value)
    {
        if (value.Length == 0)
        {
            return "\"\"";
        }

        bool needsQuotes = value.IndexOfAny(new char[] { ' ', '\t', '\n', '\v', '"' }) >= 0;
        if (!needsQuotes)
        {
            return value;
        }

        StringBuilder result = new StringBuilder();
        result.Append('"');
        int backslashes = 0;
        foreach (char c in value)
        {
            if (c == '\\')
            {
                backslashes++;
            }
            else if (c == '"')
            {
                result.Append('\\', backslashes * 2 + 1);
                result.Append('"');
                backslashes = 0;
            }
            else
            {
                result.Append('\\', backslashes);
                result.Append(c);
                backslashes = 0;
            }
        }
        result.Append('\\', backslashes * 2);
        result.Append('"');
        return result.ToString();
    }
}
"@

    try {
        Set-Content -LiteralPath $sourcePath -Value $source -Encoding UTF8
        $args = @('/nologo', '/target:exe', '/platform:anycpu', '/optimize+', "/out:$LauncherPath", $sourcePath)
        $output = & $compiler @args 2>&1
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $LauncherPath -PathType Leaf)) {
            throw "Launcher compile failed: $($output | Out-String)"
        }
    } finally {
        Remove-Item -LiteralPath $sourcePath -Force -ErrorAction SilentlyContinue
    }
}

function Get-ExternalLaunchPath {
    param(
        [string]$ExePath,
        [string]$AliasName
    )

    $exeDir = [System.IO.Path]::GetDirectoryName($ExePath)
    $binDir = [System.IO.Path]::Combine($exeDir, 'bin')
    $aliasBase = [System.IO.Path]::GetFileNameWithoutExtension($AliasName)
    $aliasCmd = [System.IO.Path]::Combine($binDir, "$aliasBase.cmd")

    if (Test-Path -LiteralPath $aliasCmd -PathType Leaf) {
        return $aliasCmd
    }

    if (Test-Path -LiteralPath $binDir -PathType Container) {
        $exeName = [System.IO.Path]::GetFileName($ExePath)
        foreach ($cmd in Get-ChildItem -LiteralPath $binDir -Filter '*.cmd' -File -ErrorAction SilentlyContinue) {
            $text = Get-Content -LiteralPath $cmd.FullName -Raw -ErrorAction SilentlyContinue
            if ($text -like "*$exeName*") {
                return $cmd.FullName
            }
        }
    }

    return $ExePath
}

function Register-AppPaths {
    param(
        [string]$AliasName,
        [string]$TargetPath,
        [ValidateSet('User','Machine')] [string]$Scope = 'User'
    )

    $keyPath = Get-AppPathsKeyPath -AliasName $AliasName -Scope $Scope
    if (-not (Test-Path $keyPath)) {
        New-Item -Path $keyPath -Force | Out-Null
    }

    Set-ItemProperty -Path $keyPath -Name '(Default)' -Value $TargetPath
    Set-ItemProperty -Path $keyPath -Name 'Path' -Value ([System.IO.Path]::GetDirectoryName($TargetPath))
}

function Remove-AppPaths {
    param(
        [string]$AliasName,
        [ValidateSet('User','Machine')] [string]$Scope = 'User'
    )

    $keyPath = Get-AppPathsKeyPath -AliasName $AliasName -Scope $Scope
    if (Test-Path $keyPath) {
        Remove-Item $keyPath -Recurse -Force
    }
}

function New-AppAlias {
    [CmdletBinding(SupportsShouldProcess)]
    param(
        [Parameter(Mandatory)] [string]$ExePath,
        [string]$Alias = '',
        [string]$Rename = '',
        [string]$ProductCode = '',
        [ValidateSet('User','Machine')] [string]$Scope = 'User',
        [ValidateSet('x86','x64','arm64','neutral')] [string]$Arch = 'x64',
        [string]$DisplayName = '',
        [string]$DisplayVersion = '',
        [string]$Publisher = '',
        [switch]$ExternalTarget,
        [switch]$Force
    )

    Assert-ScopeWritable -Scope $Scope

    $ExePath = [System.IO.Path]::GetFullPath($ExePath)
    if (-not (Test-Path $ExePath -PathType Leaf)) {
        throw "Source exe not found: $ExePath"
    }

    $aliasName = Resolve-AliasName -ExePath $ExePath -Alias $Alias -Rename $Rename
    $productCode = if ($ProductCode) { $ProductCode } else { [System.IO.Path]::GetFileNameWithoutExtension($ExePath) }
    $display = if ($DisplayName) { $DisplayName } else { $productCode }
    $installRoot = Get-PortableInstallRoot -Scope $Scope -Arch $Arch
    $installDir = [System.IO.Path]::Combine($installRoot, $productCode)
    $linksDir = Get-PortableLinksLocation -Scope $Scope
    $symlinkPath = [System.IO.Path]::Combine($linksDir, $aliasName)
    $mode = if ($ExternalTarget) { 'External' } else { 'Portable' }
    $targetExe = if ($ExternalTarget) { $ExePath } else { [System.IO.Path]::Combine($installDir, $aliasName) }
    $launcherPath = if ($ExternalTarget) { [System.IO.Path]::Combine($installDir, $aliasName) } else { '' }
    $launchCommandPath = if ($ExternalTarget) { Get-ExternalLaunchPath -ExePath $ExePath -AliasName $aliasName } else { '' }
    $linkTarget = if ($ExternalTarget) { $launcherPath } else { $targetExe }
    $sha256 = Get-FileSHA256 -Path $ExePath
    $existing = Get-ARPEntry -ProductCode $productCode -Scope $Scope

    if ($existing -and -not $Force) {
        $sameTarget = ($existing.Target -ieq $targetExe)
        $sameMode = ($existing.Mode -ieq $mode)
        if (-not ($sameTarget -and $sameMode)) {
            throw "ProductCode collision: $productCode. Use -Force to overwrite."
        }
    }

    if (-not $PSCmdlet.ShouldProcess($aliasName, 'Create app alias')) { return }

    Register-ARPEntry `
        -ProductCode $productCode `
        -DisplayName $display `
        -DisplayVersion $DisplayVersion `
        -Publisher $Publisher `
        -InstallLocation $installDir `
        -SymlinkFullPath $symlinkPath `
        -TargetFullPath $targetExe `
        -LauncherFullPath $launcherPath `
        -LauncherCommandFullPath $launchCommandPath `
        -SHA256 $sha256 `
        -AliasMode $mode `
        -InstallDirectoryCreated $false `
        -InstallDirectoryAddedToPath $false `
        -Scope $Scope

    $installDirectoryCreated = $false
    if (-not (Test-Path $installDir)) {
        New-Item -ItemType Directory -Path $installDir -Force | Out-Null
        $installDirectoryCreated = $true
    }

    if (-not (Test-Path $linksDir)) {
        New-Item -ItemType Directory -Path $linksDir -Force | Out-Null
    }

    if ($ExternalTarget) {
        New-ExternalLauncher -LaunchPath $launchCommandPath -LauncherPath $launcherPath
    } else {
        if (Test-Path $targetExe) {
            Remove-Item $targetExe -Force
        }
        Copy-Item -LiteralPath $ExePath -Destination $targetExe -Force
    }

    $installDirectoryAddedToPath = $false
    $symlinkCreated = New-Symlink -LinkPath $symlinkPath -TargetPath $linkTarget
    if ($symlinkCreated) {
        Add-ToPathVariable -Directory $linksDir -Scope $Scope | Out-Null
    } else {
        Add-ToPathVariable -Directory $installDir -Scope $Scope | Out-Null
        $installDirectoryAddedToPath = $true
    }

    Register-ARPEntry `
        -ProductCode $productCode `
        -DisplayName $display `
        -DisplayVersion $DisplayVersion `
        -Publisher $Publisher `
        -InstallLocation $installDir `
        -SymlinkFullPath $symlinkPath `
        -TargetFullPath $targetExe `
        -LauncherFullPath $launcherPath `
        -LauncherCommandFullPath $launchCommandPath `
        -SHA256 $sha256 `
        -AliasMode $mode `
        -InstallDirectoryCreated $installDirectoryCreated `
        -InstallDirectoryAddedToPath $installDirectoryAddedToPath `
        -Scope $Scope

    Register-AppPaths -AliasName $aliasName -TargetPath $targetExe -Scope $Scope

    Write-Host "Alias '$($aliasName -replace '\.exe$')' created -> $targetExe" -ForegroundColor Green
    Write-Host "  Mode    : $mode" -ForegroundColor DarkGray
    Write-Host "  Link    : $symlinkPath" -ForegroundColor DarkGray
    Write-Host "  PATH    : $(if ($installDirectoryAddedToPath) { $installDir } else { $linksDir })" -ForegroundColor DarkGray
    Write-Host "  ARP key : Portable_$productCode" -ForegroundColor DarkGray
    Write-Host "  Restart your shell to use the alias." -ForegroundColor Yellow
}

function Remove-AppAlias {
    [CmdletBinding(SupportsShouldProcess)]
    param(
        [Parameter(Mandatory)] [string]$ProductCode,
        [string]$AliasName = '',
        [ValidateSet('User','Machine')] [string]$Scope = 'User',
        [switch]$Purge
    )

    Assert-ScopeWritable -Scope $Scope

    $entry = Get-ARPEntry -ProductCode $ProductCode -Scope $Scope
    $aliasName = if ($AliasName) { Resolve-AliasName -ExePath $AliasName -Alias $AliasName } elseif ($entry -and $entry.Alias) { $entry.Alias } else { "$ProductCode.exe" }
    $installRoot = Get-PortableInstallRoot -Scope $Scope
    $installDir = if ($entry -and $entry.InstallLocation) { $entry.InstallLocation } else { [System.IO.Path]::Combine($installRoot, $ProductCode) }
    $linksDir = Get-PortableLinksLocation -Scope $Scope
    $symlinkPath = if ($entry -and $entry.Symlink) { $entry.Symlink } else { [System.IO.Path]::Combine($linksDir, $aliasName) }
    $targetExe = if ($entry -and $entry.Target) { $entry.Target } else { [System.IO.Path]::Combine($installDir, $aliasName) }
    $launcherPath = if ($entry -and $entry.Launcher) { $entry.Launcher } else { [System.IO.Path]::Combine($installDir, $aliasName) }
    $mode = if ($entry) { $entry.Mode } else { 'Portable' }

    if (-not $PSCmdlet.ShouldProcess($aliasName, 'Remove app alias')) { return }

    if (Test-Path $symlinkPath) {
        Remove-Item $symlinkPath -Force
    }

    if ($mode -eq 'External') {
        if (Test-Path $launcherPath) {
            Remove-Item $launcherPath -Force
        }
    } elseif (Test-Path $targetExe) {
        Remove-Item $targetExe -Force
    }

    if ($entry -and $entry.InstallDirectoryAddedToPath) {
        Remove-FromPathVariable -Directory $installDir -Scope $Scope | Out-Null
    }

    if (Test-Path $installDir) {
        if ($Purge) {
            Remove-Item $installDir -Recurse -Force
        } elseif ((Get-ChildItem $installDir -Force | Measure-Object).Count -eq 0) {
            Remove-Item $installDir -Force
        } else {
            Write-Warning "Install directory not empty: $installDir"
        }
    }

    if (Test-Path $linksDir) {
        $remaining = Get-ChildItem $linksDir -Force | Measure-Object
        if ($remaining.Count -eq 0) {
            Remove-FromPathVariable -Directory $linksDir -Scope $Scope | Out-Null
        }
    }

    Remove-ARPEntry -ProductCode $ProductCode -Scope $Scope
    Remove-AppPaths -AliasName $aliasName -Scope $Scope
    Write-Host "Alias '$($aliasName -replace '\.exe$')' removed." -ForegroundColor Green
}

function Get-AppAlias {
    param([ValidateSet('User','Machine','Both')] [string]$Scope = 'Both')

    $scopes = @()
    if ($Scope -in 'User', 'Both') { $scopes += 'User' }
    if ($Scope -in 'Machine', 'Both') { $scopes += 'Machine' }

    foreach ($currentScope in $scopes) {
        $hive = if ($currentScope -eq 'Machine') { 'HKLM' } else { 'HKCU' }
        $base = "$hive`:\Software\Microsoft\Windows\CurrentVersion\Uninstall"
        if (-not (Test-Path $base)) { continue }

        Get-ChildItem $base |
            Where-Object { $_.PSChildName -like 'Portable_*' } |
            ForEach-Object {
                $productCode = $_.PSChildName -replace '^Portable_'
                $entry = Get-ARPEntry -ProductCode $productCode -Scope $currentScope
                if ($entry) {
                    [pscustomobject]@{
                        ProductCode = $entry.ProductCode
                        DisplayName = $entry.DisplayName
                        Alias = $entry.Alias
                        Target = $entry.Target
                        Launcher = $entry.Launcher
                        LaunchCommand = $entry.LaunchCommand
                        InstallLocation = $entry.InstallLocation
                        Mode = $entry.Mode
                        Scope = $entry.Scope
                    }
                }
            }
    }
}

function Get-AppPathsTarget {
    param(
        [string]$AliasName,
        [ValidateSet('User','Machine')] [string]$Scope = 'User'
    )

    $path = Get-AppPathsKeyPath -AliasName $AliasName -Scope $Scope
    if (-not (Test-Path $path)) { return '' }

    $item = Get-Item -Path $path
    return [string]$item.GetValue('')
}

function Test-AppAlias {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string]$ProductCode,
        [ValidateSet('User','Machine')] [string]$Scope = 'User'
    )

    $entry = Get-ARPEntry -ProductCode $ProductCode -Scope $Scope
    if (-not $entry) {
        return [pscustomobject]@{
            ProductCode = $ProductCode
            Scope = $Scope
            Passed = $false
            ArpExists = $false
            TargetExists = $false
            LauncherExists = $false
            SymlinkExists = $false
            SymlinkTargetMatches = $false
            PathEntryPresent = $false
            HashMatches = $false
            AppPathsExists = $false
            AppPathsTargetMatches = $false
            Mode = ''
            Target = ''
            Launcher = ''
            Symlink = ''
        }
    }

    $targetExists = ($entry.Target -ne '') -and (Test-Path -LiteralPath $entry.Target -PathType Leaf)
    $expectedLinkTarget = if ($entry.Mode -eq 'External') { $entry.Launcher } else { $entry.Target }
    $launcherExists = if ($entry.Mode -eq 'External') {
        ($entry.Launcher -ne '') -and (Test-Path -LiteralPath $entry.Launcher -PathType Leaf)
    } else {
        $targetExists
    }

    $symlinkExists = ($entry.Symlink -ne '') -and (Test-Path -LiteralPath $entry.Symlink -PathType Leaf)
    $symlinkTargetMatches = $false
    if ($symlinkExists) {
        $actualTarget = (Get-Item -LiteralPath $entry.Symlink).Target
        if ($actualTarget -is [array]) { $actualTarget = $actualTarget[0] }
        $symlinkTargetMatches = ([string]$actualTarget -ieq [string]$expectedLinkTarget)
    }

    $pathToCheck = if ($entry.InstallDirectoryAddedToPath) {
        $entry.InstallLocation
    } else {
        Get-PortableLinksLocation -Scope $Scope
    }
    $key = Get-EnvironmentPathKey -Scope $Scope
    try {
        $pathValue = [string]$key.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        $pathEntryPresent = Test-PathValueContains -PathValue $pathValue -Directory $pathToCheck
    } finally {
        $key.Close()
    }

    $hashMatches = $true
    if ($entry.SHA256) {
        $hashMatches = $false
        if ($targetExists) {
            $hashMatches = ((Get-FileSHA256 -Path $entry.Target) -ieq $entry.SHA256)
        }
    }

    $appPathsTarget = Get-AppPathsTarget -AliasName $entry.Alias -Scope $Scope
    $appPathsExists = $appPathsTarget -ne ''
    $appPathsTargetMatches = $appPathsExists -and ($appPathsTarget -ieq $entry.Target)

    $passed = $targetExists -and
        $launcherExists -and
        $symlinkExists -and
        $symlinkTargetMatches -and
        $pathEntryPresent -and
        $hashMatches -and
        $appPathsExists -and
        $appPathsTargetMatches

    [pscustomobject]@{
        ProductCode = $entry.ProductCode
        Scope = $entry.Scope
        Passed = [bool]$passed
        ArpExists = $true
        TargetExists = [bool]$targetExists
        LauncherExists = [bool]$launcherExists
        SymlinkExists = [bool]$symlinkExists
        SymlinkTargetMatches = [bool]$symlinkTargetMatches
        PathEntryPresent = [bool]$pathEntryPresent
        HashMatches = [bool]$hashMatches
        AppPathsExists = [bool]$appPathsExists
        AppPathsTargetMatches = [bool]$appPathsTargetMatches
        Mode = $entry.Mode
        Target = $entry.Target
        Launcher = $entry.Launcher
        Symlink = $entry.Symlink
    }
}

Export-ModuleMember -Function New-AppAlias, Remove-AppAlias, Get-AppAlias, Test-AppAlias, Add-ToPathVariable, Remove-FromPathVariable
