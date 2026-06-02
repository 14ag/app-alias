#Requires -Version 5.1
<#
.SYNOPSIS
    Programmatic App Execution Alias generator.

.DESCRIPTION
    Reworked from winget-cli PortableInstaller.cpp + PortableFlow.cpp (commit c8fe1ea0).
    Replicates the full portable install pipeline without requiring MSIX or winget:

      PortableFlow.cpp  -> ResolveAliasName(), New-AppAlias entry-point
      PortableInstaller -> Install-File, Add-ToPathVariable, Register-ARPEntry,
                          Create-TargetDirectory, CreateSymlink w/ PATH fallback

    Source:
      https://github.com/microsoft/winget-cli/blob/c8fe1ea0/src/AppInstallerCLICore/PortableInstaller.cpp
      https://github.com/microsoft/winget-cli/blob/c8fe1ea0/src/AppInstallerCLICore/Workflows/PortableFlow.cpp
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Region: Path constants  (mirrors Runtime::PathName resolution in winget)
# ---------------------------------------------------------------------------

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

# ---------------------------------------------------------------------------
# Region: Alias name resolution  (mirrors GetDesiredStateForPortableInstall)
# ---------------------------------------------------------------------------

function Resolve-AliasName {
    <#
    .DESCRIPTION
        Mirrors the alias resolution priority in PortableFlow.cpp:
          1. Explicit -Alias parameter  (equivalent to manifest Commands[0])
          2. -Rename parameter          (equivalent to --rename CLI arg)
          3. Original exe filename      (fallback)
        Always ensures .exe extension (Filesystem::AppendExtension).
    #>
    param(
        [string]$ExePath,
        [string]$Alias   = '',   # manifest Commands[0] equivalent
        [string]$Rename  = ''    # --rename equivalent
    )

    $name = [System.IO.Path]::GetFileNameWithoutExtension($ExePath)

    if ($Alias)  { $name = [System.IO.Path]::GetFileNameWithoutExtension($Alias) }
    if ($Rename) { $name = [System.IO.Path]::GetFileNameWithoutExtension($Rename) }

    # Filesystem::AppendExtension(commandAlias, ".exe")
    return $name + '.exe'
}

# ---------------------------------------------------------------------------
# Region: PATH variable management  (mirrors AddToPathVariable / RemoveFromPathVariable)
# ---------------------------------------------------------------------------

function Add-ToPathVariable {
    param(
        [string]$Directory,
        [ValidateSet('User','Machine')] [string]$Scope = 'User'
    )

    $target   = if ($Scope -eq 'Machine') { [System.EnvironmentVariableTarget]::Machine }
                else                       { [System.EnvironmentVariableTarget]::User    }
    $current  = [System.Environment]::GetEnvironmentVariable('PATH', $target)
    $parts    = $current -split ';' | Where-Object { $_ -ne '' }

    if ($parts -notcontains $Directory) {
        $newPath = ($parts + $Directory) -join ';'
        [System.Environment]::SetEnvironmentVariable('PATH', $newPath, $target)

        # Broadcast WM_SETTINGCHANGE so explorer/shells pick up the change immediately
        # (mirrors PathVariable::Append broadcasting in winget)
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

        Write-Verbose "Appended '$Directory' to $Scope PATH - shell restart may be required."
        return $true
    }
    Write-Verbose "'$Directory' already in $Scope PATH."
    return $false
}

function Remove-FromPathVariable {
    param(
        [string]$Directory,
        [ValidateSet('User','Machine')] [string]$Scope = 'User'
    )

    $target  = if ($Scope -eq 'Machine') { [System.EnvironmentVariableTarget]::Machine }
               else                       { [System.EnvironmentVariableTarget]::User    }
    $current = [System.Environment]::GetEnvironmentVariable('PATH', $target)
    $parts   = $current -split ';' | Where-Object { $_ -ne '' -and $_ -ne $Directory }
    [System.Environment]::SetEnvironmentVariable('PATH', ($parts -join ';'), $target)
    Write-Verbose "Removed '$Directory' from $Scope PATH."
}

# ---------------------------------------------------------------------------
# Region: Symlink creation  (mirrors Filesystem::CreateSymlink + fallback logic)
# ---------------------------------------------------------------------------

function New-Symlink {
    <#
    .DESCRIPTION
        Attempts to create a filesystem symlink (requires SeCreateSymbolicLinkPrivilege
        or Developer Mode on Win10+).  Returns $true on success, $false on failure
        so the caller can fall back to adding the install dir to PATH instead -
        exactly as PortableInstaller::InstallFile() does.
    #>
    param(
        [string]$LinkPath,
        [string]$TargetPath
    )

    # Remove stale link/file at destination (mirrors std::filesystem::remove(filePath))
    if (Test-Path $LinkPath) {
        Remove-Item $LinkPath -Force
        Write-Verbose "Removed existing item at '$LinkPath'."
    }

    try {
        # Use cmd mklink so we don't depend on PowerShell version for symlink support
        $null = & cmd /c mklink "$LinkPath" "$TargetPath" 2>&1
        if ($LASTEXITCODE -eq 0 -and (Test-Path $LinkPath)) {
            Write-Verbose "Symlink created: '$LinkPath' -> '$TargetPath'"
            return $true
        }
        return $false
    } catch {
        return $false
    }
}

# ---------------------------------------------------------------------------
# Region: ARP (Add/Remove Programs) registry  (mirrors PortableARPEntry)
# ---------------------------------------------------------------------------

function Register-ARPEntry {
    <#
    .DESCRIPTION
        Writes the Uninstall registry key that makes the alias appear in
        Settings -> Apps and in the ARP list.
        Mirrors PortableInstaller::RegisterARPEntry() + CommitToARPEntry().

        Key path:
          HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Portable_{ProductCode}
          HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Portable_{ProductCode}  (Machine scope)
    #>
    param(
        [string]$ProductCode,
        [string]$DisplayName,
        [string]$Publisher       = '',
        [string]$InstallLocation,
        [string]$SymlinkFullPath,
        [string]$TargetFullPath,
        [string]$WinGetPackageId = '',
        [string]$WinGetSourceId  = '',
        [ValidateSet('User','Machine')] [string]$Scope = 'User'
    )

    $hive    = if ($Scope -eq 'Machine') { 'HKLM' } else { 'HKCU' }
    $keyPath = "$hive`:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Portable_$ProductCode"

    if (-not (Test-Path $keyPath)) {
        New-Item -Path $keyPath -Force | Out-Null
    }

    $values = @{
        DisplayName               = $DisplayName
        Publisher                 = $Publisher
        InstallLocation           = $InstallLocation
        PortableTargetFullPath    = $TargetFullPath
        PortableSymlinkFullPath   = $SymlinkFullPath
        WinGetPackageIdentifier   = $WinGetPackageId
        WinGetSourceIdentifier    = $WinGetSourceId
        UninstallString           = "powershell -Command `"Remove-AppAlias -ProductCode '$ProductCode' -Scope $Scope`""
        NoModify                  = 1
        NoRepair                  = 1
    }

    foreach ($entry in $values.GetEnumerator()) {
        if ($entry.Value -ne '') {
            $kind = if ($entry.Value -is [int]) { 'DWord' } else { 'String' }
            Set-ItemProperty -Path $keyPath -Name $entry.Key -Value $entry.Value -Type $kind
        }
    }

    Write-Verbose "ARP entry registered at: $keyPath"
}

function Remove-ARPEntry {
    param(
        [string]$ProductCode,
        [ValidateSet('User','Machine')] [string]$Scope = 'User'
    )
    $hive    = if ($Scope -eq 'Machine') { 'HKLM' } else { 'HKCU' }
    $keyPath = "$hive`:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Portable_$ProductCode"
    if (Test-Path $keyPath) {
        Remove-Item $keyPath -Recurse -Force
        Write-Verbose "ARP entry removed: $keyPath"
    }
}

# ---------------------------------------------------------------------------
# Region: App Paths registry  (mirrors App Paths registration for ShellExecute/Win+R)
# ---------------------------------------------------------------------------

function Register-AppPaths {
    param(
        [string]$AliasName,   # e.g. "mytool.exe"
        [string]$TargetPath,
        [ValidateSet('User','Machine')] [string]$Scope = 'User'
    )
    $hive    = if ($Scope -eq 'Machine') { 'HKLM' } else { 'HKCU' }
    $keyPath = "$hive`:\Software\Microsoft\Windows\CurrentVersion\App Paths\$AliasName"
    if (-not (Test-Path $keyPath)) { New-Item -Path $keyPath -Force | Out-Null }
    Set-ItemProperty -Path $keyPath -Name '(Default)' -Value $TargetPath
    Set-ItemProperty -Path $keyPath -Name 'Path'      -Value ([System.IO.Path]::GetDirectoryName($TargetPath))
    Write-Verbose "App Paths registered: $keyPath -> $TargetPath"
}

function Remove-AppPaths {
    param(
        [string]$AliasName,
        [ValidateSet('User','Machine')] [string]$Scope = 'User'
    )
    $hive    = if ($Scope -eq 'Machine') { 'HKLM' } else { 'HKCU' }
    $keyPath = "$hive`:\Software\Microsoft\Windows\CurrentVersion\App Paths\$AliasName"
    if (Test-Path $keyPath) {
        Remove-Item $keyPath -Recurse -Force
        Write-Verbose "App Paths entry removed: $keyPath"
    }
}

# ---------------------------------------------------------------------------
# Region: Public API - New-AppAlias (Install)
# ---------------------------------------------------------------------------

function New-AppAlias {
    <#
    .SYNOPSIS
        Creates a programmatic app execution alias for any .exe.

    .DESCRIPTION
        Full port of winget's PortableInstaller pipeline:
          1. Resolve alias name
          2. Register ARP entry (before file ops, so failure is recoverable)
          3. Create target install directory
          4. Copy exe to install root
          5. Create symlink in links directory  -> fallback to PATH if symlink fails
          6. Ensure links directory is on PATH
          7. Register App Paths (Win+R / ShellExecute support)

    .PARAMETER ExePath
        Full path to the source executable.

    .PARAMETER Alias
        Override the alias name (equivalent to manifest Commands[0]).
        Defaults to the exe's filename.

    .PARAMETER Rename
        Rename both the installed exe and the alias (equivalent to winget --rename).
        Takes precedence over -Alias.

    .PARAMETER ProductCode
        Unique identifier used for the install subdirectory and ARP key.
        Defaults to the exe's base name.

    .PARAMETER Scope
        'User' (default, no elevation) or 'Machine' (requires admin).

    .PARAMETER DisplayName
        Human-readable name shown in Apps & Features. Defaults to ProductCode.

    .PARAMETER Publisher
        Publisher string for the ARP entry.

    .EXAMPLE
        New-AppAlias -ExePath 'C:\Tools\ripgrep.exe' -Alias 'rg'

    .EXAMPLE
        New-AppAlias -ExePath 'C:\Tools\fzf.exe' -Scope Machine -Publisher 'Junegunn Choi'
    #>
    [CmdletBinding(SupportsShouldProcess)]
    param(
        [Parameter(Mandatory)] [string]$ExePath,
        [string]$Alias        = '',
        [string]$Rename       = '',
        [string]$ProductCode  = '',
        [ValidateSet('User','Machine')] [string]$Scope = 'User',
        [string]$DisplayName  = '',
        [string]$Publisher    = ''
    )

    # --- Validate source
    $ExePath = [System.IO.Path]::GetFullPath($ExePath)
    if (-not (Test-Path $ExePath -PathType Leaf)) {
        throw "Source exe not found: $ExePath"
    }

    # --- Resolve names  (PortableFlow: GetDesiredStateForPortableInstall alias logic)
    $aliasName   = Resolve-AliasName -ExePath $ExePath -Alias $Alias -Rename $Rename
    $productCode = if ($ProductCode) { $ProductCode }
                   else { [System.IO.Path]::GetFileNameWithoutExtension($ExePath) }
    $displayName = if ($DisplayName) { $DisplayName } else { $productCode }

    # --- Resolve directories
    $installRoot  = Get-PortableInstallRoot -Scope $Scope
    $installDir   = [System.IO.Path]::Combine($installRoot, $productCode)
    $linksDir     = Get-PortableLinksLocation -Scope $Scope
    $targetExe    = [System.IO.Path]::Combine($installDir, $aliasName)
    $symlinkPath  = [System.IO.Path]::Combine($linksDir, $aliasName)

    Write-Verbose "Alias name    : $aliasName"
    Write-Verbose "Product code  : $productCode"
    Write-Verbose "Install dir   : $installDir"
    Write-Verbose "Links dir     : $linksDir"
    Write-Verbose "Target exe    : $targetExe"
    Write-Verbose "Symlink path  : $symlinkPath"

    if (-not $PSCmdlet.ShouldProcess($aliasName, 'Create app alias')) { return }

    # --- Step 1: Register ARP BEFORE file ops (mirrors Install() ordering for fresh installs)
    Register-ARPEntry `
        -ProductCode   $productCode `
        -DisplayName   $displayName `
        -Publisher     $Publisher `
        -InstallLocation $installDir `
        -SymlinkFullPath $symlinkPath `
        -TargetFullPath  $targetExe `
        -Scope           $Scope

    # --- Step 2: Create target install directory  (CreateTargetInstallDirectory)
    if (-not (Test-Path $installDir)) {
        New-Item -ItemType Directory -Path $installDir -Force | Out-Null
        Write-Verbose "Created install directory: $installDir"
    }

    # --- Step 3: Create links directory (needed for symlinks)
    if (-not (Test-Path $linksDir)) {
        New-Item -ItemType Directory -Path $linksDir -Force | Out-Null
        Write-Verbose "Created links directory: $linksDir"
    }

    # --- Step 4: Copy exe  (InstallFile for PortableFileType::File)
    if (Test-Path $targetExe) {
        Remove-Item $targetExe -Force
        Write-Verbose "Removed existing target exe."
    }
    Copy-Item -Path $ExePath -Destination $targetExe -Force
    Write-Verbose "Copied exe to: $targetExe"

    # --- Step 5: Symlink with PATH fallback  (InstallFile for PortableFileType::Symlink)
    $symlinkCreated = New-Symlink -LinkPath $symlinkPath -TargetPath $targetExe

    if ($symlinkCreated) {
        Write-Verbose "Alias symlink active: $symlinkPath"
        # --- Step 6: Ensure links dir is on PATH  (AddToPathVariable(GetPortableLinksLocation))
        Add-ToPathVariable -Directory $linksDir -Scope $Scope | Out-Null
    } else {
        # Fallback: add the install dir directly to PATH
        # (mirrors: AddToPathVariable(symlinkTargetPath.parent_path()))
        Write-Warning "Symlink creation failed (requires Developer Mode or elevated prompt). Falling back to adding install directory to PATH."
        Add-ToPathVariable -Directory $installDir -Scope $Scope | Out-Null
    }

    # --- Step 7: App Paths (Win+R / ShellExecute)
    Register-AppPaths -AliasName $aliasName -TargetPath $targetExe -Scope $Scope

    Write-Host "✓ Alias '$($aliasName -replace '\.exe$')' created -> $targetExe" -ForegroundColor Green
    if ($symlinkCreated) {
        Write-Host "  Symlink : $symlinkPath" -ForegroundColor DarkGray
    }
    Write-Host "  PATH    : $linksDir" -ForegroundColor DarkGray
    Write-Host "  ARP key : Portable_$productCode" -ForegroundColor DarkGray
    Write-Host "  Restart your shell to use the alias." -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
# Region: Public API - Remove-AppAlias (Uninstall)
# ---------------------------------------------------------------------------

function Remove-AppAlias {
    <#
    .SYNOPSIS
        Removes an app alias created by New-AppAlias.

    .DESCRIPTION
        Mirrors PortableInstaller::Uninstall():
          1. Remove symlink or undo PATH addition
          2. Remove copied exe
          3. Remove install directory (if empty)
          4. Remove links dir from PATH (if now unused)
          5. Delete ARP entry
          6. Delete App Paths entry

    .PARAMETER ProductCode
        The ProductCode used when the alias was created.

    .PARAMETER AliasName
        The alias filename (e.g. 'rg.exe'). If omitted, defaults to ProductCode + '.exe'.

    .PARAMETER Scope
        Scope used at install time.

    .PARAMETER Purge
        If set, removes the install directory and all its contents regardless of whether
        it is empty (equivalent to winget uninstall --purge).
    #>
    [CmdletBinding(SupportsShouldProcess)]
    param(
        [Parameter(Mandatory)] [string]$ProductCode,
        [string]$AliasName = '',
        [ValidateSet('User','Machine')] [string]$Scope = 'User',
        [switch]$Purge
    )

    $aliasName   = if ($AliasName) { $AliasName } else { "$ProductCode.exe" }
    $installRoot = Get-PortableInstallRoot -Scope $Scope
    $installDir  = [System.IO.Path]::Combine($installRoot, $productCode)
    $linksDir    = Get-PortableLinksLocation -Scope $Scope
    $symlinkPath = [System.IO.Path]::Combine($linksDir, $aliasName)
    $targetExe   = [System.IO.Path]::Combine($installDir, $aliasName)

    if (-not $PSCmdlet.ShouldProcess($aliasName, 'Remove app alias')) { return }

    # Remove symlink  (RemoveFile for PortableFileType::Symlink)
    if (Test-Path $symlinkPath) {
        Remove-Item $symlinkPath -Force
        Write-Verbose "Removed symlink: $symlinkPath"
    }

    # Remove exe  (RemoveFile for PortableFileType::File)
    if (Test-Path $targetExe) {
        Remove-Item $targetExe -Force
        Write-Verbose "Removed exe: $targetExe"
    }

    # Remove install directory  (RemoveInstallDirectory)
    if (Test-Path $installDir) {
        if ($Purge) {
            Remove-Item $installDir -Recurse -Force
            Write-Verbose "Purged install directory: $installDir"
        } elseif ((Get-ChildItem $installDir -Force | Measure-Object).Count -eq 0) {
            Remove-Item $installDir -Force
            Write-Verbose "Removed empty install directory: $installDir"
        } else {
            Write-Warning "Install directory not empty, leaving in place: $installDir"
        }
    }

    # Remove links dir from PATH only if it is now empty  (RemoveFromPathVariable)
    if (Test-Path $linksDir) {
        $remaining = Get-ChildItem $linksDir -Force | Measure-Object
        if ($remaining.Count -eq 0) {
            Remove-FromPathVariable -Directory $linksDir -Scope $Scope
        }
    }

    # Remove ARP entry
    Remove-ARPEntry -ProductCode $productCode -Scope $Scope

    # Remove App Paths entry
    Remove-AppPaths -AliasName $aliasName -Scope $Scope

    Write-Host "✓ Alias '$($aliasName -replace '\.exe$')' removed." -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# Region: Public API - Get-AppAlias (List)
# ---------------------------------------------------------------------------

function Get-AppAlias {
    <#
    .SYNOPSIS
        Lists all app aliases registered by AliasManager (Portable_ ARP entries).
    #>
    param([ValidateSet('User','Machine','Both')] [string]$Scope = 'Both')

    $hives = @()
    if ($Scope -in 'User',  'Both') { $hives += 'HKCU' }
    if ($Scope -in 'Machine','Both') { $hives += 'HKLM' }

    foreach ($hive in $hives) {
        $base = "$hive`:\Software\Microsoft\Windows\CurrentVersion\Uninstall"
        if (-not (Test-Path $base)) { continue }
        Get-ChildItem $base |
            Where-Object { $_.PSChildName -like 'Portable_*' } |
            ForEach-Object {
                $props = Get-ItemProperty $_.PSPath
                [PSCustomObject]@{
                    ProductCode    = $_.PSChildName -replace '^Portable_'
                    DisplayName    = $props.DisplayName
                    Alias          = [System.IO.Path]::GetFileName($props.PortableSymlinkFullPath)
                    Target         = $props.PortableTargetFullPath
                    InstallLocation= $props.InstallLocation
                    Scope          = $(if ($hive -eq 'HKCU') { 'User' } else { 'Machine' })
                }
            }
    }
}

Export-ModuleMember -Function New-AppAlias, Remove-AppAlias, Get-AppAlias
