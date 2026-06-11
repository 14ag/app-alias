#Requires -Version 5.1

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$modulePath = Join-Path $repoRoot 'AliasManager.psm1'

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-Equal {
    param($Expected, $Actual, [string]$Message)
    if ($Expected -ne $Actual) {
        throw "$Message Expected='$Expected' Actual='$Actual'"
    }
}

function Invoke-Test {
    param([string]$Name, [scriptblock]$Body)

    try {
        & $Body
        [pscustomobject]@{ Name = $Name; Result = 'PASS'; Message = '' }
    } catch {
        [pscustomobject]@{ Name = $Name; Result = 'FAIL'; Message = $_.Exception.Message }
    }
}

$results = @()

$results += Invoke-Test 'ExternalTarget alias launches original executable and cleans up generated artifacts' {
    Import-Module $modulePath -Force

    $productCode = 'AliasManagerTestExternal'
    $alias = 'alias-test-external'
    $aliasName = "$alias.exe"
    $sourceExe = (Get-Command powershell.exe -ErrorAction Stop).Source
    $installDir = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages\$productCode"
    $linksDir = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links'
    $launcherPath = Join-Path $installDir $aliasName
    $linkPath = Join-Path $linksDir $aliasName
    $arpPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Portable_$productCode"
    $appPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\App Paths\$aliasName"

    try {
        Remove-AppAlias -ProductCode $productCode -AliasName $aliasName -Scope User -Purge -ErrorAction SilentlyContinue | Out-Null
        if (Test-Path $arpPath) { Remove-Item -Path $arpPath -Recurse -Force }
        if (Test-Path $appPath) { Remove-Item -Path $appPath -Recurse -Force }
        if (Test-Path $linkPath) { Remove-Item -LiteralPath $linkPath -Force }
        if (Test-Path $installDir) { Remove-Item -LiteralPath $installDir -Recurse -Force }

        New-AppAlias -ExePath $sourceExe -Alias $alias -ProductCode $productCode -DisplayName $productCode -ExternalTarget | Out-Null

        $item = Get-AppAlias -Scope User | Where-Object { $_.ProductCode -eq $productCode } | Select-Object -First 1
        Assert-True ($null -ne $item) 'Get-AppAlias did not return external alias.'
        Assert-Equal $aliasName $item.Alias 'Alias mismatch.'
        Assert-Equal $sourceExe $item.Target 'External target must stay original executable path.'
        Assert-Equal 'External' $item.Mode 'Mode mismatch.'

        $props = Get-ItemProperty -Path $arpPath
        Assert-Equal $sourceExe $props.TargetFullPath 'Winget TargetFullPath must store original external target.'
        Assert-Equal $linkPath $props.SymlinkFullPath 'Winget SymlinkFullPath mismatch.'
        Assert-Equal $launcherPath $props.LauncherFullPath 'LauncherFullPath mismatch.'

        Assert-True (Test-Path -LiteralPath $launcherPath -PathType Leaf) 'External launcher exe missing.'
        Assert-True (Test-Path -LiteralPath $linkPath -PathType Leaf) 'Alias link missing.'
        $linkTarget = (Get-Item -LiteralPath $linkPath).Target
        Assert-Equal $launcherPath $linkTarget 'Alias link must target launcher, not original exe.'

        $output = & $linkPath -NoProfile -Command "[Console]::Out.Write('alias-ok')"
        Assert-Equal 'alias-ok' $output 'External launcher did not pass arguments to original executable.'

        Remove-AppAlias -ProductCode $productCode -AliasName $aliasName -Scope User -Purge | Out-Null
        Assert-True (Test-Path -LiteralPath $sourceExe -PathType Leaf) 'Original executable was removed.'
        Assert-True (-not (Test-Path -LiteralPath $launcherPath)) 'Launcher still exists after remove.'
        Assert-True (-not (Test-Path -LiteralPath $linkPath)) 'Alias link still exists after remove.'
        Assert-True (-not (Test-Path $arpPath)) 'ARP key still exists after remove.'
    } finally {
        Remove-AppAlias -ProductCode $productCode -AliasName $aliasName -Scope User -Purge -ErrorAction SilentlyContinue | Out-Null
        if (Test-Path $arpPath) { Remove-Item -Path $arpPath -Recurse -Force -ErrorAction SilentlyContinue }
        if (Test-Path $appPath) { Remove-Item -Path $appPath -Recurse -Force -ErrorAction SilentlyContinue }
        if (Test-Path $linkPath) { Remove-Item -LiteralPath $linkPath -Force -ErrorAction SilentlyContinue }
        if (Test-Path $installDir) { Remove-Item -LiteralPath $installDir -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

$results += Invoke-Test 'PATH registry writer preserves expandable value type' {
    Import-Module $modulePath -Force

    $testDir = Join-Path $env:TEMP 'AliasManagerPathTypeTest'
    $envKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $true)
    $originalPath = $envKey.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    $originalKind = $envKey.GetValueKind('Path')

    try {
        Add-ToPathVariable -Directory $testDir -Scope User | Out-Null
        $kind = $envKey.GetValueKind('Path')
        Assert-Equal ([Microsoft.Win32.RegistryValueKind]::ExpandString) $kind 'User PATH value kind mismatch.'
    } finally {
        $envKey.SetValue('Path', $originalPath, $originalKind)
        $envKey.Close()
    }
}

$results += Invoke-Test 'Test-AppAlias reports healthy and broken alias state' {
    Import-Module $modulePath -Force

    $productCode = 'AliasManagerTestVerify'
    $alias = 'alias-test-verify'
    $aliasName = "$alias.exe"
    $sourceExe = (Get-Command powershell.exe -ErrorAction Stop).Source
    $installDir = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages\$productCode"
    $linksDir = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links'
    $linkPath = Join-Path $linksDir $aliasName
    $arpPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Portable_$productCode"
    $appPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\App Paths\$aliasName"

    try {
        Remove-AppAlias -ProductCode $productCode -AliasName $aliasName -Scope User -Purge -ErrorAction SilentlyContinue | Out-Null
        if (Test-Path $arpPath) { Remove-Item -Path $arpPath -Recurse -Force }
        if (Test-Path $appPath) { Remove-Item -Path $appPath -Recurse -Force }
        if (Test-Path $linkPath) { Remove-Item -LiteralPath $linkPath -Force }
        if (Test-Path $installDir) { Remove-Item -LiteralPath $installDir -Recurse -Force }

        New-AppAlias -ExePath $sourceExe -Alias $alias -ProductCode $productCode -DisplayName $productCode -ExternalTarget | Out-Null

        $healthy = Test-AppAlias -ProductCode $productCode -Scope User
        Assert-True $healthy.Passed 'Healthy alias verification failed.'
        Assert-True $healthy.TargetExists 'TargetExists false for healthy alias.'
        Assert-True $healthy.SymlinkTargetMatches 'SymlinkTargetMatches false for healthy alias.'
        Assert-True $healthy.PathEntryPresent 'PathEntryPresent false for healthy alias.'
        Assert-True $healthy.HashMatches 'HashMatches false for healthy alias.'

        Remove-Item -LiteralPath $linkPath -Force
        $broken = Test-AppAlias -ProductCode $productCode -Scope User
        Assert-True (-not $broken.Passed) 'Broken alias verification passed.'
        Assert-True (-not $broken.SymlinkExists) 'Broken alias still reports symlink exists.'
    } finally {
        Remove-AppAlias -ProductCode $productCode -AliasName $aliasName -Scope User -Purge -ErrorAction SilentlyContinue | Out-Null
        if (Test-Path $arpPath) { Remove-Item -Path $arpPath -Recurse -Force -ErrorAction SilentlyContinue }
        if (Test-Path $appPath) { Remove-Item -Path $appPath -Recurse -Force -ErrorAction SilentlyContinue }
        if (Test-Path $linkPath) { Remove-Item -LiteralPath $linkPath -Force -ErrorAction SilentlyContinue }
        if (Test-Path $installDir) { Remove-Item -LiteralPath $installDir -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

$results += Invoke-Test 'alias.bat add supports ExternalTarget switch' {
    $productCode = 'AliasManagerTestBatchExternal'
    $alias = 'alias-test-batch-external'
    $aliasName = "$alias.exe"
    $sourceDir = Join-Path $env:TEMP 'Alias Manager Batch Source'
    $sourceExe = Join-Path $sourceDir 'Power Shell Test.exe'
    $installDir = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages\$productCode"
    $linksDir = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links'
    $linkPath = Join-Path $linksDir $aliasName
    $arpPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Portable_$productCode"
    $appPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\App Paths\$aliasName"
    $aliasBat = Join-Path $repoRoot 'alias.bat'

    try {
        Import-Module $modulePath -Force
        Remove-AppAlias -ProductCode $productCode -AliasName $aliasName -Scope User -Purge -ErrorAction SilentlyContinue | Out-Null
        if (-not (Test-Path $sourceDir)) { New-Item -ItemType Directory -Path $sourceDir -Force | Out-Null }
        Copy-Item -LiteralPath (Get-Command powershell.exe -ErrorAction Stop).Source -Destination $sourceExe -Force
        if (Test-Path $arpPath) { Remove-Item -Path $arpPath -Recurse -Force }
        if (Test-Path $appPath) { Remove-Item -Path $appPath -Recurse -Force }
        if (Test-Path $linkPath) { Remove-Item -LiteralPath $linkPath -Force }
        if (Test-Path $installDir) { Remove-Item -LiteralPath $installDir -Recurse -Force }

        $output = & $aliasBat add -ExePath $sourceExe -Alias $alias -ProductCode $productCode -DisplayName 'Alias Manager Batch Test' -Publisher 'Test Publisher' -ExternalTarget -Force 2>&1
        if ($LASTEXITCODE -ne 0) { throw ($output | Out-String) }
        if (($output | Out-String) -match 'New-AppAlias\s*:|CategoryInfo|FullyQualifiedErrorId') { throw ($output | Out-String) }

        $check = Test-AppAlias -ProductCode $productCode -Scope User
        Assert-True $check.Passed 'alias.bat-created alias failed verification.'
    } finally {
        Import-Module $modulePath -Force
        Remove-AppAlias -ProductCode $productCode -AliasName $aliasName -Scope User -Purge -ErrorAction SilentlyContinue | Out-Null
        if (Test-Path $arpPath) { Remove-Item -Path $arpPath -Recurse -Force -ErrorAction SilentlyContinue }
        if (Test-Path $appPath) { Remove-Item -Path $appPath -Recurse -Force -ErrorAction SilentlyContinue }
        if (Test-Path $linkPath) { Remove-Item -LiteralPath $linkPath -Force -ErrorAction SilentlyContinue }
        if (Test-Path $installDir) { Remove-Item -LiteralPath $installDir -Recurse -Force -ErrorAction SilentlyContinue }
        if (Test-Path $sourceDir) { Remove-Item -LiteralPath $sourceDir -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

$results += Invoke-Test 'alias.bat handles VS Code Insiders external target path' {
    $vsCodeExe = 'C:\Program Files\Microsoft VS Code Insiders\Code - Insiders.exe'
    if (-not (Test-Path -LiteralPath $vsCodeExe -PathType Leaf)) { return }

    $productCode = 'AliasManagerTestVSCodeExternal'
    $alias = 'alias-test-vscode-external'
    $aliasName = "$alias.exe"
    $installDir = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages\$productCode"
    $linksDir = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links'
    $linkPath = Join-Path $linksDir $aliasName
    $arpPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Portable_$productCode"
    $appPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\App Paths\$aliasName"
    $aliasBat = Join-Path $repoRoot 'alias.bat'

    try {
        Import-Module $modulePath -Force
        Remove-AppAlias -ProductCode $productCode -AliasName $aliasName -Scope User -Purge -ErrorAction SilentlyContinue | Out-Null
        if (Test-Path $arpPath) { Remove-Item -Path $arpPath -Recurse -Force }
        if (Test-Path $appPath) { Remove-Item -Path $appPath -Recurse -Force }
        if (Test-Path $linkPath) { Remove-Item -LiteralPath $linkPath -Force }
        if (Test-Path $installDir) { Remove-Item -LiteralPath $installDir -Recurse -Force }

        $output = & $aliasBat add -ExePath $vsCodeExe -Alias $alias -ProductCode $productCode -DisplayName 'Microsoft Visual Studio Code Insiders' -Publisher 'Microsoft Corporation' -ExternalTarget -Force 2>&1
        if ($LASTEXITCODE -ne 0) { throw ($output | Out-String) }
        if (($output | Out-String) -match 'New-AppAlias\s*:|CategoryInfo|FullyQualifiedErrorId') { throw ($output | Out-String) }

        $check = Test-AppAlias -ProductCode $productCode -Scope User
        Assert-True $check.Passed 'VS Code Insiders external alias failed verification.'

        $actualVersion = (& $linkPath --version | Select-Object -First 1)
        $expectedVersion = (& 'C:\Program Files\Microsoft VS Code Insiders\bin\code-insiders.cmd' --version | Select-Object -First 1)
        Assert-Equal $expectedVersion $actualVersion 'VS Code Insiders alias must match official CLI version output.'
    } finally {
        Import-Module $modulePath -Force
        Remove-AppAlias -ProductCode $productCode -AliasName $aliasName -Scope User -Purge -ErrorAction SilentlyContinue | Out-Null
        if (Test-Path $arpPath) { Remove-Item -Path $arpPath -Recurse -Force -ErrorAction SilentlyContinue }
        if (Test-Path $appPath) { Remove-Item -Path $appPath -Recurse -Force -ErrorAction SilentlyContinue }
        if (Test-Path $linkPath) { Remove-Item -LiteralPath $linkPath -Force -ErrorAction SilentlyContinue }
        if (Test-Path $installDir) { Remove-Item -LiteralPath $installDir -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

$results | Format-Table -AutoSize

$failed = @($results | Where-Object { $_.Result -ne 'PASS' })
if ($failed.Count -gt 0) {
    throw "$($failed.Count) test(s) failed."
}
