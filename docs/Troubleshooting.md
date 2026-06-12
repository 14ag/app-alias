# Troubleshooting

Start with the CLI. These two commands catch most bad states:

```powershell
.\build\Release\AppAlias.Cli.exe list
.\build\Release\AppAlias.Cli.exe verify --alias chrome-appalias.exe
```

If a command says the alias is healthy but Settings still looks stale, close Settings and open it again. It caches app metadata.

## Alias does not appear in Settings

First check that the package installed and that the WindowsApps stub exists:

```powershell
.\build\Release\AppAlias.Cli.exe verify --alias chrome-appalias.exe
```

Expected result:

```text
StubExists: true
StubIsAppExecLink: true
```

If the package is missing, rerun `create`. If the stub is missing, don't guess. Check the deployment log:

```powershell
Get-WinEvent -LogName Microsoft-Windows-AppXDeploymentServer/Operational -MaxEvents 50
```

## Stub exists but launch fails

Check the reparse tag:

```powershell
fsutil reparsepoint query "$env:LOCALAPPDATA\Microsoft\WindowsApps\chrome-appalias.exe"
```

Expected tag:

```text
0x8000001b
```

Then check AppModel activation logs:

```powershell
Get-WinEvent -LogName Microsoft-Windows-AppModel-Runtime/Admin -MaxEvents 50
```

Known causes:

- Package identity contains punctuation from the alias stem. Recreate with current code; it strips punctuation.
- Proxy was built with dynamic Visual C++ runtime dependencies. Rebuild with the project CMake settings.
- Signing certificate is not trusted by the current user.

## Blank icon in Settings

Confirm the package contains PNG assets:

```powershell
$pkg = Get-AppxPackage -Name 'AppAliasGenerator.chromeappalias.*'
Get-ChildItem -LiteralPath (Join-Path $pkg.InstallLocation 'Assets') -Filter '*.png'
```

If assets exist, close and reopen Settings. If the icon still doesn't change, recreate the alias with a new package version or package identity so Settings reloads metadata.

```powershell
.\build\Release\AppAlias.Cli.exe create --alias chrome-appalias.exe --target "C:\Program Files\Google\Chrome\Application\chrome.exe" --package-version 1.0.0.1 --force
```

## Signing or trust failure

Run preflight:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\Test-AppAliasPrereqs.ps1
```

Create a local cert:

```powershell
$cert = powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\New-AppAliasCert.ps1 | ConvertFrom-Json
$env:APPALIAS_PUBLISHER_SUBJECT = $cert.Subject
$env:APPALIAS_CERT_SHA1 = $cert.Thumbprint
```

For an existing cert, set:

```powershell
$env:APPALIAS_PUBLISHER_SUBJECT = 'CN=YourCertSubject'
$env:APPALIAS_CERT_SHA1 = '<thumbprint>'
$env:APPALIAS_CERT_STORE = 'CurrentUser'
```

The package publisher and certificate subject must match.

For PFX signing, set both values. The tool does not use a default PFX password.

```powershell
$env:APPALIAS_PFX = 'C:\path\signing.pfx'
$env:APPALIAS_PFX_PASSWORD = '<password>'
```

If `Get-AuthenticodeSignature` says the MSIX signature is valid but deployment fails with `0x800B0109`, Windows does not trust the signing root for package deployment. On this machine that required machine-level trust. Run the cert helper from elevated Windows PowerShell with `-Machine`, or use a signing cert already trusted under LocalMachine:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\New-AppAliasCert.ps1 -Machine -Force
```

The dev helper `.vscode\get_admin.bat` prompts for elevation and returns the elevated command exit code. `scripts\Install-AppAliasCertMachine.ps1` uses it for machine-level cert trust.

## Remove refuses alias

The tool removes only owned packages. Owned package names start with:

```text
AppAliasGenerator.
```

Aliases owned by packages such as `Microsoft.DesktopAppInstaller` are foreign. Removing those rows means removing or replacing the owning package, not deleting one file from WindowsApps.

`create --force` also refuses a foreign alias. `remove` accepts either `--alias` or `--package`, not both.

## CLI exit codes

```text
0 success
1 operation failed
2 usage error
3 alias or package not found
4 foreign alias blocked
5 stub invalid
6 exception
```

## Appx cmdlets fail in PowerShell 7

On this machine, `Get-AppxPackage` has failed from PowerShell 7. Use Windows PowerShell for Appx module checks:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Get-AppxPackage -Name 'AppAliasGenerator*'"
```
