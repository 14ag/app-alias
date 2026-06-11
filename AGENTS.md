# AGENTS.md

Project: Windows App Execution Alias generator.

## Commands

- Preflight tools:
  `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\Test-AppAliasPrereqs.ps1`
- Build native targets and run CTest:
  `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\Build-AppAlias.ps1`
- Legacy PowerShell tests:
  `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\AliasManager.Tests.ps1`
- Create current-user test cert:
  `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\New-AppAliasCert.ps1`
- Use existing trusted cert for package signing:
  `$env:APPALIAS_PUBLISHER_SUBJECT='CN=<subject>'; $env:APPALIAS_CERT_SHA1='<thumbprint>'`
- Create Chrome test alias:
  `.\build\Release\AppAlias.Cli.exe create --alias chrome-appalias.exe --target "C:\Program Files\Google\Chrome\Application\chrome.exe" --display-name ChromeAppAlias --publisher AppAliasGenerator --force`
- Verify alias:
  `.\build\Release\AppAlias.Cli.exe verify --alias chrome-appalias.exe`
- List aliases:
  `.\build\Release\AppAlias.Cli.exe list --json`
- Remove owned alias:
  `.\build\Release\AppAlias.Cli.exe remove --alias chrome-appalias.exe`

## Architecture

- `src\AppAlias.Core`: C++17/C++WinRT core for identity, manifest, MSIX packing/signing, deployment, list, verify, remove.
- `src\AppAlias.Proxy`: packaged console launcher. Reads `alias.json` beside itself and forwards args to target exe/cmd.
- `src\AppAlias.Cli`: `appalias create/list/remove/verify`.
- `src\AppAlias.Ui`: native Win32 UI shell. It is not WinUI 3 yet.
- New Settings-visible aliases use signed full MSIX packages via `PackageManager.AddPackageByUriAsync`.
- Do not use symlink, App Paths, PATH, or legacy PowerShell fallback for Settings-visible aliases.

## MSIX Alias Contract

- Manifest must use `EntryPoint="Windows.FullTrustApplication"` and `runFullTrust`.
- Current working alias schema is `uap5:Extension Category="windows.appExecutionAlias"` with `uap5:ExecutionAlias`.
- Package `ProcessorArchitecture` must be `x64` for the native proxy.
- Package contains `AppAlias.Proxy.exe` and `alias.json`.
- Build proxy with static CRT. Dynamic `MSVCP140.dll` / `VCRUNTIME140.dll` dependencies can break AppModel activation.
- Do not request `unvirtualizedResources`; proxy does not need it.
- Sparse package / `ExternalLocationUri` path failed on this machine for `windows.appExecutionAlias` with `0x80070032`.

## Identity Rules

- Alias may contain hyphen, but package identity must not preserve punctuation from alias stem.
- Use alphanumeric alias stem for package name:
  `chrome-appalias.exe` -> `AppAliasGenerator.chromeappalias.<hash8>`.
- Hyphenated package identity created an AppExecLink stub but failed activation with `The system cannot execute the specified program`.
- App id stays `AliasApp`.
- Default publisher subject is `CN=AppAliasGenerator`; package publisher must match signing cert subject.

## Remove Policy

- Tool removes only packages it owns: package name starts with `AppAliasGenerator.`.
- `remove --alias <name.exe>` finds installed alias, then removes the owning package if owned.
- `remove --package <name-or-full-name>` removes owned package by name or full name.
- Foreign aliases are listed but not removed.
- `python.exe` and `python3.exe` are owned by `Microsoft.DesktopAppInstaller`; removing those rows means removing or replacing App Installer, not deleting a stub.

## Verification

- `verify` must confirm package manifest alias plus WindowsApps AppExecLink stub.
- Use `GetFileAttributesW` as fallback; `std::filesystem::exists` can return false for AppExecLink stubs.
- Confirm stub tag:
  `fsutil reparsepoint query "$env:LOCALAPPDATA\Microsoft\WindowsApps\<alias>.exe"`
- Expected tag: `0x8000001b`.
- Confirm shell resolution:
  `cmd.exe /d /c where <alias>.exe`
- Chrome blackbox test:
  `cmd.exe /d /c "<alias>.exe --headless=new --disable-gpu --user-data-dir=%TEMP%\appalias-chrome-test --dump-dom data:text/html,appalias-ok"`

## Icons

- Settings page shows alias row from package manifest and AppExecLink state.
- Create path extracts the target file icon via Shell APIs and writes package PNG assets.
- Expected assets: `Assets\StoreLogo.png`, `Assets\Square44x44Logo.png`, `Assets\Square150x150Logo.png`, plus `Square44x44Logo.targetsize-*` variants.
- If Settings keeps an old blank icon, close/reopen Settings first. If cache persists, bump package version or package name, then recreate alias.

## Gotchas

- CurrentUser self-signed cert in Root may install package but can still fail activation on some machines; add cert to TrustedPeople/TrustedPublisher as needed.
- `Get-AppxPackage` can fail in PowerShell 7 on this machine; use `powershell.exe` Windows PowerShell for Appx module commands.
- AppModel activation failures surface in `Microsoft-Windows-AppModel-Runtime/Admin`.
- Deployment/alias registration errors surface in `Microsoft-Windows-AppXDeploymentServer/Operational`.
- Event `AppExecutionAlias directory missing, error code is 0x8007010B` can appear during remove/re-register flows; validate final package and stub state before fixing.
