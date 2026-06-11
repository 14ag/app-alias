# Contributing

This repository supports one alias path: native C++ MSIX packages that register `windows.appExecutionAlias`.

## Setup

1. Install Visual Studio 2022 Build Tools with C++ x64 tools.
2. Install the Windows SDK tools that include `makeappx.exe` and `signtool.exe`.
3. Install CMake.
4. Check the machine:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\Test-AppAliasPrereqs.ps1
```

5. Create or configure a signing certificate:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\New-AppAliasCert.ps1
```

For an existing cert:

```powershell
$env:APPALIAS_PUBLISHER_SUBJECT = 'CN=YourCertSubject'
$env:APPALIAS_CERT_SHA1 = '<thumbprint>'
```

## Build and test

Run the full native build:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\Build-AppAlias.ps1
```

That command builds Release targets and runs `AppAliasCore.Tests`.

When touching alias creation, also run a live verify against an installed alias:

```powershell
.\build\Release\AppAlias.Cli.exe verify --alias chrome-appalias.exe
```

## Code rules

- Keep `AppAlias.Core` responsible for package identity, manifests, deployment, and verification.
- Keep `AppAlias.Proxy` small. It should read `alias.json`, forward arguments, and return the target process exit code.
- Build native binaries with static CRT. MSIX activation can fail when the proxy depends on undeclared Visual C++ runtime DLLs.
- Do not add symlink, `App Paths`, or `PATH` fallback behavior for Settings-visible aliases.
- Keep package identity alphanumeric inside the alias stem. Alias names may contain hyphens; package names should not.
- Request only `runFullTrust` unless a concrete feature needs another capability.

## Documentation rules

- Document commands that have been tested in this repository.
- Document the native MSIX path as the supported workflow.
- Put long-form behavior notes under `docs/`.
- Keep troubleshooting entries symptom-first: what the user sees, what it means, and what to run.

## Pull request checklist

- Build passes with `scripts\Build-AppAlias.ps1`.
- New behavior has a focused test in `tests\AppAliasCore.Tests.cpp`.
- CLI examples still match `src\AppAlias.Cli\main.cpp`.
- Docs mention any new signing, deployment, or Settings behavior.
- Foreign aliases are not removed by new code.

## Commit messages

Use short imperative commit messages. Conventional prefixes such as `fix:`, `docs:`, and `feat:` are fine when they help.
