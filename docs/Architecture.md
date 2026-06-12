# Architecture

The core trick is one MSIX package per alias. We don't write the WindowsApps stub ourselves. Windows Deployment does that after it reads `windows.appExecutionAlias` from the package manifest.

Each package carries a proxy executable and an `alias.json` file. The proxy is boring on purpose: read target path, rebuild arguments, launch target.

## Components

- `AppAlias.Core`: normalizes aliases, builds package identity, writes manifests, stages package files, signs MSIX packages, deploys them, and verifies WindowsApps stubs.
- `AppAlias.Cli`: exposes `create`, `list`, `remove`, and `verify`.
- `AppAlias.Proxy`: runs inside the installed package and launches the configured target with the original arguments.
- `AppAlias.Ui`: native Win32 UI shell for listing, verifying, and removing aliases.

That's the split. Most rules live in Core because Settings-visible aliases are package behavior, not shell shortcut behavior.

## Create flow

1. CLI receives `--alias`, `--target`, display name, publisher name, and `--force`.
2. Core normalizes the alias and builds a deterministic package name.
3. Core stages package files under `%LOCALAPPDATA%\AppAliasGenerator\Packages\<packageName>`.
4. The staged package contains `AppAlias.Proxy.exe`, `alias.json`, `AppxManifest.xml`, and PNG icon assets extracted from the target executable.
5. `makeappx.exe` packs the MSIX.
6. `signtool.exe` signs it.
7. `PackageManager.AddPackageByUriAsync` registers the package for the current user.
8. Windows Deployment creates `%LOCALAPPDATA%\Microsoft\WindowsApps\<alias>.exe` as an AppExecLink stub.

The manifest uses `EntryPoint="Windows.FullTrustApplication"`, `runFullTrust`, `ProcessorArchitecture="x64"`, and `uap5:Extension Category="windows.appExecutionAlias"`. Don't switch this back to the old sparse-package path; that failed on this machine.

## Launch flow

When a user runs the alias, Windows follows the `0x8000001b` AppExecLink stub and activates the package application. The package starts `AppAlias.Proxy.exe`. The proxy reads `alias.json`, rebuilds the command line from the original arguments, starts the target process, waits for it, and returns the target exit code.

For `.cmd` and `.bat` targets, the proxy runs the full `%SystemRoot%\System32\cmd.exe` path. For `.ps1` targets, it runs the full Windows PowerShell path. For executable targets, it launches the target path directly. It does not resolve targets through `PATH`.

## Icon flow

Core extracts the target executable icon and writes PNG files into `Assets\` before packing the MSIX. Icon extraction can touch shell/icon APIs, so UI callers keep it away from the message thread. The package includes:

- `StoreLogo.png`
- `Square44x44Logo.png`
- `Square150x150Logo.png`
- `Square44x44Logo.targetsize-*` variants

Settings reads icon metadata from the installed package. If Settings keeps an older blank icon, the cache may need a Settings restart or a new package identity.

## Identity and ownership

Package names use:

```text
AppAliasGenerator.<alphanumericAliasStem>.<hash8>
```

The alias itself can keep punctuation such as hyphens. The package name does not. This avoids an activation failure seen when package identity preserved hyphens from the alias stem.

The tool treats packages starting with `AppAliasGenerator.` as owned. It lists foreign aliases but refuses to remove them.
