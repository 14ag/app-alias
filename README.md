# App Alias Generator

Need an alias to show up in Windows Settings? App Alias Generator builds a signed MSIX package for it. It doesn't use symlinks, `App Paths`, or `PATH` edits.

Each package carries one alias. It includes a small proxy executable, an `alias.json` file with the target path, and PNG icon assets extracted from the target executable. Windows Deployment creates the `%LOCALAPPDATA%\Microsoft\WindowsApps\<alias>.exe` AppExecLink stub, then Settings reads the alias from the package manifest.

## Features

- Create current-user app execution aliases that appear in Settings.
- Use `windows.appExecutionAlias` with `Windows.FullTrustApplication`.
- Extract target executable icons into MSIX visual assets.
- List owned and foreign package aliases.
- Remove only packages owned by this tool.
- Verify `0x8000001b` AppExecLink stubs.

## Prerequisites

- Windows desktop with MSIX deployment support.
- Visual Studio 2022 Build Tools with C++ x64 tools.
- Windows SDK tools: `makeappx.exe` and `signtool.exe`.
- CMake.
- A code-signing certificate trusted by the current user.

Check tools first:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\Test-AppAliasPrereqs.ps1
```

Create a current-user test certificate:

```powershell
$cert = powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\New-AppAliasCert.ps1 | ConvertFrom-Json
$env:APPALIAS_PUBLISHER_SUBJECT = $cert.Subject
$env:APPALIAS_CERT_SHA1 = $cert.Thumbprint
```

You can also sign with an existing cert:

```powershell
$env:APPALIAS_PUBLISHER_SUBJECT = 'CN=YourCertSubject'
$env:APPALIAS_CERT_SHA1 = '<thumbprint>'
$env:APPALIAS_CERT_STORE = 'CurrentUser'
```

The package publisher has to match the signing certificate subject. If it doesn't, deployment fails before the alias can be registered.

PFX signing is explicit. Set both variables when using a PFX:

```powershell
$env:APPALIAS_PFX = "$env:LOCALAPPDATA\AppAliasGenerator\Cert\AppAliasGenerator.pfx"
$env:APPALIAS_PFX_PASSWORD = '<password>'
```

## Build

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\Build-AppAlias.ps1
```

The script configures a Visual Studio 2022 x64 build, builds Release targets, and runs CTest.

## CLI usage

Run the CLI from the Release build output:

```powershell
$appalias = ".\build\Release\AppAlias.Cli.exe"
```

Create an alias:

```powershell
& $appalias create --alias chrome-appalias.exe --target "C:\Program Files\Google\Chrome\Application\chrome.exe" --display-name ChromeAppAlias --publisher AppAliasGenerator --package-version 1.0.0.0 --force
```

List aliases:

```powershell
& $appalias list
& $appalias list --json
```

Plain `list` output is tab-delimited: alias, package name, owner state. JSON output returns these fields:

```text
alias, packageName, packageFamilyName, packageFullName, target,
installedPackagePath, stagedMsixPath, externalLocation, owned,
stubExists, stubIsAppExecLink
```

Verify an alias:

```powershell
& $appalias verify --alias chrome-appalias.exe
```

Remove an owned alias:

```powershell
& $appalias remove --alias chrome-appalias.exe
```

The tool refuses to remove foreign package aliases. For example, `python.exe` and `python3.exe` are owned by `Microsoft.DesktopAppInstaller` on many systems, so this tool reports them as foreign.

`remove` accepts either `--alias` or `--package`, not both. `create --force` replaces only aliases owned by this tool; it refuses foreign aliases before deployment.

CLI exit codes:

```text
0 success
1 operation failed
2 usage error
3 alias or package not found
4 foreign alias blocked
5 stub invalid
6 exception
```

## Tested examples

```powershell
& $appalias create --alias code-insiders.exe --target "C:\Program Files\Microsoft VS Code Insiders\Code - Insiders.exe" --display-name "Visual Studio Code Insiders" --publisher AppAliasGenerator --force
& $appalias create --alias steam.exe --target "C:\Program Files (x86)\Steam\steam.exe" --display-name Steam --publisher AppAliasGenerator --force
& $appalias create --alias joyxoff.exe --target "C:\Program Files (x86)\Joyxoff\Joyxoff.exe" --display-name JoyXoff --publisher AppAliasGenerator --force
```

Verify the WindowsApps stub:

```powershell
fsutil reparsepoint query "$env:LOCALAPPDATA\Microsoft\WindowsApps\steam.exe"
```

Expected tag:

```text
0x8000001b
```

## Project layout

- `src/AppAlias.Core`: package identity, manifest generation, MSIX packing/signing, deployment, list, verify, remove.
- `src/AppAlias.Proxy`: packaged launcher that forwards arguments to the target program.
- `src/AppAlias.Cli`: command line interface.
- `src/AppAlias.Ui`: native Win32 UI shell.
- `tests/AppAliasCore.Tests.cpp`: core tests.
- `scripts/`: build, preflight, and certificate helpers.
- `docs/`: architecture and troubleshooting notes.

## Known behavior

- Package names strip punctuation from alias stems. `chrome-appalias.exe` becomes `AppAliasGenerator.chromeappalias.<hash>`. This avoids AppModel activation failures seen with hyphenated package names.
- Settings can cache old icons. Close and reopen Settings after recreating an alias. If the icon still does not change, recreate the alias with `--package-version` set to a new version or use a new package identity.
- Icon extraction reads the target executable during create. UI callers run create/verify/remove away from the message thread so shell and icon APIs do not freeze the window.
## Documentation

- [Architecture](docs/Architecture.md)
- [Troubleshooting](docs/Troubleshooting.md)
- [Contributing](CONTRIBUTING.md)

## License

No license file is present yet. Treat the code as not licensed for redistribution until a license is added.
