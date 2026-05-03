# App Execution Alias Generator — Development Documentation

**Project:** Programmatic App Execution Alias Generator for Windows
**Source Research:** winget-cli commit `c8fe1ea0` (microsoft/winget-cli)
**Artifact:** `AliasManager.psm1`
**Status:** Phase 1 complete — core install/uninstall/list pipeline working
**Next Phase:** Agent-driven CLI, cross-scope support, C++ native port option

---

## Table of Contents

1. [Project Summary](#1-project-summary)
2. [Windows Alias Mechanisms — Full Taxonomy](#2-windows-alias-mechanisms--full-taxonomy)
3. [How MSIX App Execution Aliases Actually Work](#3-how-msix-app-execution-aliases-actually-work)
4. [Winget Source Analysis](#4-winget-source-analysis)
5. [AliasManager.psm1 — Design Decisions](#5-aliasmanagerpsm1--design-decisions)
6. [Known Constraints and Edge Cases](#6-known-constraints-and-edge-cases)
7. [Registry Schema Reference](#7-registry-schema-reference)
8. [Path and Directory Reference](#8-path-and-directory-reference)
9. [Implementation Task Breakdown](#9-implementation-task-breakdown)
10. [Agent Instructions](#10-agent-instructions)

---

## 1. Project Summary

### Goal

Create a standalone, scriptable system that registers any arbitrary `.exe` as a
globally resolvable command alias on Windows — matching the behavior of a winget
portable package install — without requiring MSIX packaging, the winget client,
or any Microsoft Store dependency.

### What "App Execution Alias" Means in Context

The term covers three distinct Windows mechanisms that are commonly conflated:

| Mechanism | What it is | Requires MSIX |
|---|---|---|
| MSIX AppExecutionAlias | Reparse-point stub + kernel activation | Yes |
| App Paths registry | Name → full path for `ShellExecute` and `Win+R` | No |
| Symlink on PATH | `.exe` symlink in a PATH directory | No |
| Copy on PATH | Actual binary copy in a PATH directory | No |

The current module implements the **winget portable approach**: symlink in a
dedicated links directory + App Paths registration. This is the practical ceiling
for a user-space tool that targets arbitrary exes.

### Completed Work

- Full reverse-engineering of winget's `PortableInstaller.cpp` and `PortableFlow.cpp`
- PowerShell module `AliasManager.psm1` with `New-AppAlias`, `Remove-AppAlias`, `Get-AppAlias`
- Accurate port of: alias name resolution, symlink creation with PATH fallback,
  ARP entry registration, App Paths registration, PATH variable management with
  `WM_SETTINGCHANGE` broadcast

---

## 2. Windows Alias Mechanisms — Full Taxonomy

### 2.1 MSIX App Execution Alias (kernel-level)

**How it works:**

1. Package manifest declares `<desktop:ExecutionAlias Alias="tool.exe" />`
2. MSIX deployment engine places a stub PE at
   `%LOCALAPPDATA%\Microsoft\WindowsApps\tool.exe`
3. The stub is not a real executable — it carries the reparse tag
   `IO_REPARSE_TAG_APPEXECLINK` (`0x8000001B`)
4. When `CreateProcess` is called on the stub, the kernel's reparse handler
   intercepts it and calls `ActivateApplication` via `IApplicationActivationManager`
5. The actual binary runs inside the MSIX trust boundary with VFS and registry
   virtualization active

**Why you cannot fake this:**
`IO_REPARSE_TAG_APPEXECLINK` can only be written by the deployment engine operating
on a package with a valid identity. There is no Win32 API to set this tag manually.

**The `%LOCALAPPDATA%\Microsoft\WindowsApps` directory** is added to `%PATH%`
automatically by Windows on first MSIX install. So any file placed in this directory
is immediately resolvable from any shell without further PATH manipulation.

### 2.2 Sparse Package (closest legitimate reproduction)

Windows 10 1903+ (`build 18362+`) introduced sparse packages: MSIX manifests
applied to unpackaged apps. This grants the app a real package identity, enabling
genuine `AppExecutionAlias` stub creation while the actual binary remains outside
the package.

Registration:
```powershell
Add-AppxPackage -Path ".\AppxManifest.xml" `
                -ExternalLocation "C:\Tools" `
                -AllowUnsigned
```

The manifest must declare:
```xml
<Application Id="App" Executable="C:\Tools\mytool.exe"
             EntryPoint="Windows.FullTrustApplication">
  <Extensions>
    <uap3:Extension Category="windows.appExecutionAlias">
      <uap3:AppExecutionAlias>
        <desktop:ExecutionAlias Alias="mytool.exe" />
      </uap3:AppExecutionAlias>
    </uap3:Extension>
  </Extensions>
</Application>
```

**Trade-off:** Requires a signing certificate or `AllowUnsigned` (dev machines
only). The resulting alias appears in Settings → Apps → App execution aliases
with a toggle. This is the right approach if the alias must survive system updates
or appear in the Settings UI.

### 2.3 App Paths Registry (ShellExecute resolution)

```
HKCU\Software\Microsoft\Windows\CurrentVersion\App Paths\mytool.exe
  (Default) = C:\Tools\mytool.exe
  Path      = C:\Tools\
```

- Enables `Win+R` → `mytool` to launch the exe
- Enables `ShellExecute("mytool.exe")` from any process
- Does NOT enable resolution from `cmd.exe` or PowerShell by name alone
- Works without PATH changes
- No admin required for `HKCU`

### 2.4 Symlink on PATH (winget's approach)

The winget portable installer creates:
- A copy of the exe in `%LOCALAPPDATA%\Microsoft\WinGet\Packages\{ProductCode}\`
- A symlink in `%LOCALAPPDATA%\Microsoft\WinGet\Links\` pointing to the copy
- Adds the `Links\` directory to the user PATH

This gives full `cmd.exe` + PowerShell + `ShellExecute` resolution with no
package identity. The symlink requires Developer Mode or elevation; without it,
winget falls back to adding the install directory itself to PATH.

### 2.5 Comparison Matrix

| Capability | MSIX Alias | Sparse Pkg | App Paths | Symlink+PATH | Copy+PATH |
|---|---|---|---|---|---|
| `cmd.exe` by name | Yes | Yes | No | Yes | Yes |
| PowerShell by name | Yes | Yes | No | Yes | Yes |
| `Win+R` | Yes | Yes | Yes | Partial | Partial |
| Settings UI toggle | Yes | Yes | No | No | No |
| No admin needed | Yes (user scope) | Dev mode | HKCU only | Dev mode or admin | Yes |
| Works for any exe | No | Yes | Yes | Yes | Yes |
| Updates survive | Yes | Yes | No | No | No |
| Package identity | Yes | Yes | No | No | No |

---

## 3. How MSIX App Execution Aliases Actually Work

### 3.1 Deployment Pipeline

When `PackageManager::AddPackageAsync` processes a manifest with
`AppExecutionAlias`:

1. **Manifest parsing:** `uap3:AppExecutionAlias` → `desktop:ExecutionAlias`
   entries are read by `AppxManifestObject`
2. **Stub generation:** The deployment engine writes a small PE (~3 KB) to
   `%LOCALAPPDATA%\Microsoft\WindowsApps\{Alias}.exe`
3. **Reparse point attachment:** The engine calls `DeviceIoControl` with
   `FSCTL_SET_REPARSE_POINT` using the `AppExecLinkReparseBuffer` structure
   containing the `PackageFamilyName`, `EntryPoint`, and `ExecutablePath`
4. **Registry registration:**
   ```
   HKCU\Software\Classes\Local Settings\Software\Microsoft\Windows\
     CurrentVersion\AppModel\PackageRepository\Extensions\
     windows.appExecutionAlias\{PackageFamilyName}\{AppId}
   ```
5. **Activation on launch:** `CreateProcess` on the stub triggers the kernel
   reparse handler → `wm.exe` (Windows runtime broker) → `IApplicationActivationManager`
   → full MSIX activation with VFS, registry redirection, and lifecycle management

### 3.2 The Reparse Buffer Structure

```c
typedef struct _AppExecLinkReparseBuffer {
    ULONG  ReparseTag;           // 0x8000001B
    USHORT ReparseDataLength;
    USHORT Reserved;
    ULONG  StringCount;
    WCHAR  StringList[];         // PackageFamilyName\0 EntryPoint\0 ExecutablePath\0
} AppExecLinkReparseBuffer;
```

This structure is not publicly documented in Windows SDK headers. It is referenced
in NT kernel source leaks and reverse-engineered implementations. You cannot call
`DeviceIoControl(FSCTL_SET_REPARSE_POINT)` with this tag outside of a process
that has been granted the appropriate deployment trust level.

### 3.3 Why the Stub PE Is Not Enough

Even if you copy the stub PE from `WindowsApps\` to another location, the reparse
point data is attached to the file on the filesystem, not embedded in the PE.
The activation only works because:
- The file lives in the package's VFS-mapped location
- The NTFS reparse handler reads the buffer and looks up the package registry
- The package registry entry links back to the actual binary in
  `C:\Program Files\WindowsApps\{PackageFullName}\`

---

## 4. Winget Source Analysis

### 4.1 Source Files

All source is from commit `c8fe1ea0` of `microsoft/winget-cli`:

```
src/AppInstallerCLICore/PortableInstaller.cpp   (544 lines)
src/AppInstallerCLICore/PortableInstaller.h     (120 lines)
src/AppInstallerCLICore/Workflows/PortableFlow.cpp  (340 lines)
```

### 4.2 PortableFlow.cpp — Decision Layer

This file is responsible for deciding **what** to install (the desired state),
not how to install it. Key functions:

**`GetPortableProductCode(context)`**
```cpp
return MakeSuitablePathPart(packageId + "_" + source);
```
ProductCode = sanitized `{PackageId}_{SourceIdentifier}`. For local installs,
source defaults to `"*DefaultSource"`.

**`GetDesiredStateForPortableInstall(context)`**
Builds a `vector<PortableFileEntry>` with two entries for single-exe installs:
1. `PortableFileEntry::CreateFileEntry(installerPath, targetFullPath, {})` — the exe
2. `PortableFileEntry::CreateSymlinkEntry(symlinkDir / commandAlias, targetFullPath)` — the link

Alias name resolution priority (exactly as implemented in `Resolve-AliasName`):
```
if (!commands.empty())   commandAlias = ConvertToUTF16(commands[0]);
if (!renameArg.empty())  commandAlias = ConvertToUTF16(renameArg);
Filesystem::AppendExtension(commandAlias, ".exe");
```

**`EnsureVolumeSupportsReparsePoints(context)`**
Checks that the links directory volume supports reparse points. If not, the
install is aborted. This check is not currently replicated in `AliasManager.psm1`
but should be added (see task 3.1).

**`VerifyPackageAndSourceMatch(context)`**
Guards against accidentally overwriting another package's ARP entry by checking
that the existing registry `WinGetPackageIdentifier` matches. The `--force`
flag bypasses this. Currently implemented in `Remove-ARPEntry` implicitly but
not as an explicit pre-install guard.

### 4.3 PortableInstaller.cpp — Execution Layer

**`Install(OperationType operation)`**
```
if (Install):  RegisterARPEntry() FIRST    ← so catastrophic failures are recoverable
               CreateTargetInstallDirectory()
               ApplyDesiredState()
               AddToPathVariable(linksDir)
if (Upgrade):  RegisterARPEntry() LAST     ← so failed upgrades can be retried
```
This ordering is intentional and critical. For new installs, the ARP key must
exist before file operations so the system knows to attempt cleanup if the process
dies mid-install.

**`InstallFile(entry)` — Symlink branch**
```cpp
if (BinariesDependOnPath && !InstallDirectoryAddedToPath) {
    // ArchiveBinariesDependOnPath manifest field: skip symlinks entirely
    AddToPathVariable(installDirectory);
    CommitToARPEntry(InstallDirectoryAddedToPath, true);
} else if (!InstallDirectoryAddedToPath) {
    if (CreateSymlink(symlinkTarget, filePath)) {
        // success
    } else {
        // fallback: add install dir to PATH directly
        AddToPathVariable(symlinkTarget.parent_path());
        CommitToARPEntry(InstallDirectoryAddedToPath, true);
    }
}
```
The `InstallDirectoryAddedToPath` boolean in the ARP entry is the signal used
during uninstall to know whether to remove a symlink or to remove a PATH entry.

**`AddToPathVariable(value)`**
Calls `PathVariable(scope).Append(value)` which writes to:
- User:    `HKCU\Environment\Path`
- Machine: `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment\Path`

Then broadcasts `WM_SETTINGCHANGE` with `lParam = "Environment"` so running
shells can pick up the change without restart. The PowerShell module replicates
this via `SendMessageTimeout` to `HWND_BROADCAST`.

**`RemoveFromPathVariable(value)`**
Checks if the directory exists and is non-empty before removing from PATH.
If the directory still has files (from another alias in the same links dir),
it is not removed. This guards against removing the shared links directory
when only one alias of many is being uninstalled.

**`SetAppsAndFeaturesMetadata`**
Writes `DisplayName`, `DisplayVersion`, `Publisher`, `UninstallString` to the
ARP key from the manifest's `AppsAndFeaturesEntries`. The `UninstallString` is
what Windows uses when a user clicks "Uninstall" in Settings → Apps.

### 4.4 ARP Registry Values Written by Winget

The following values are written to
`HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\Portable_{ProductCode}`:

| Value Name | Type | Content |
|---|---|---|
| `DisplayName` | REG_SZ | Human-readable app name |
| `DisplayVersion` | REG_SZ | Version string |
| `Publisher` | REG_SZ | Publisher name |
| `InstallLocation` | REG_SZ | Full path to install directory |
| `PortableTargetFullPath` | REG_SZ | Full path to installed exe |
| `PortableSymlinkFullPath` | REG_SZ | Full path to symlink in links dir |
| `WinGetPackageIdentifier` | REG_SZ | Package ID from manifest |
| `WinGetSourceIdentifier` | REG_SZ | Source ID (e.g. `winget`) |
| `InstallDirectoryCreated` | REG_DWORD | 1 if winget created the dir |
| `InstallDirectoryAddedToPath` | REG_DWORD | 1 if install dir on PATH (no symlink) |
| `UninstallString` | REG_SZ | Command to run for uninstall |
| `NoModify` | REG_DWORD | 1 |
| `NoRepair` | REG_DWORD | 1 |
| `SHA256` | REG_SZ | Hex SHA256 of the installed exe |

The `SHA256` value is used by `VerifyExpectedState()` before upgrade or uninstall
to detect if the user has modified the file and warn accordingly.

### 4.5 Portable Index (archive installs only)

For archive-based portable packages (`.zip` containing multiple executables),
winget creates a SQLite database at:
```
{InstallLocation}\{ProductCode}.db
```
via the `PortableIndex` class. This tracks all `PortableFileEntry` objects
(files, symlinks, directories) so they can all be removed during uninstall.
The current module does not need this for single-exe installs but will need it
if multi-exe archive support is added (see task 5).

---

## 5. AliasManager.psm1 — Design Decisions

### 5.1 What Was Ported Directly

| Winget behavior | Module implementation | Notes |
|---|---|---|
| `Resolve-AliasName` priority order | `Resolve-AliasName` function | Exact match |
| `CreateSymlink` + PATH fallback | `New-Symlink` + `Add-ToPathVariable` | `cmd /c mklink` used instead of Win32 directly |
| ARP key before file ops | `Register-ARPEntry` called first in `New-AppAlias` | Preserves recovery ordering |
| `WM_SETTINGCHANGE` broadcast | `SendMessageTimeout` via `Add-Type` | Enables live PATH update without shell restart |
| `RemoveFromPathVariable` guards | Links dir empty check before PATH removal | Prevents removing shared links dir |
| Machine vs User scope | `-Scope` parameter throughout | Drives hive and directory selection |

### 5.2 What Was Added Beyond Winget

- **App Paths registration** (`Register-AppPaths`): winget does not do this for
  portable installs. Added because it enables `Win+R` and `ShellExecute` without
  extra PATH manipulation.
- **`Get-AppAlias`**: winget has no equivalent list command for portable installs
  separate from `winget list`.
- **`-DisplayName` and `-Publisher` parameters**: winget reads these from the
  YAML manifest; the module exposes them directly.

### 5.3 What Was Intentionally Omitted

- **SHA256 verification on uninstall**: winget's `VerifyExpectedState()` hashes
  the installed file and refuses to proceed if it has been modified (without
  `--force`). Omitted for simplicity in v1. Should be added in task 3.3.
- **`EnsureVolumeSupportsReparsePoints`**: winget checks that the links directory
  volume supports reparse points before attempting symlink creation. The module
  falls back silently instead. Should be added as a preflight check.
- **Upgrade ordering** (ARP after file ops): only Install ordering is implemented.
  Upgrade path needs `Update-AppAlias` with the reversed ARP timing.
- **PortableIndex (SQLite tracking for archives)**: not needed for single-exe scope.

### 5.4 Symlink Strategy Detail

```
New-Symlink attempts:   cmd /c mklink "{linksDir}\{alias}.exe" "{installDir}\{alias}.exe"

Success path:
  symlink created → add linksDir to PATH

Failure path (no Dev Mode, no elevation):
  add installDir to PATH directly
  set InstallDirectoryAddedToPath = 1 in ARP
```

The `cmd /c mklink` approach is used rather than `[System.IO.File]::CreateSymbolicLink`
because the .NET method requires a newer runtime version and has inconsistent
behavior on Windows 10 vs 11. `mklink` is reliable across all target OS versions.

An alternative is `New-Item -ItemType SymbolicLink` (PowerShell 5.1+ on Win10+)
which would avoid spawning `cmd.exe`, but has the same privilege requirements.

---

## 6. Known Constraints and Edge Cases

### 6.1 Symlink Privilege

Creating symlinks on Windows requires one of:
- Developer Mode enabled (Settings → For Developers → Developer Mode)
- `SeCreateSymbolicLinkPrivilege` (default for admins)
- Running as administrator

Without this, the module falls back to adding the install directory to PATH.
The fallback works but means the alias only resolves if the install directory
is on PATH — not the clean symlink approach.

**Detection:** Check for Developer Mode via:
```powershell
(Get-ItemProperty HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock `
    -ErrorAction SilentlyContinue).AllowDevelopmentWithoutDevLicense -eq 1
```

### 6.2 Machine Scope Requires Elevation

`-Scope Machine` writes to `HKLM` and `C:\Program Files\WinGet\` — both require
administrator rights. The module does not currently check for elevation before
attempting machine-scope operations. A preflight elevation check should be added.

Detection:
```powershell
([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
```

### 6.3 PATH Length Limit

Windows has a legacy 2048-character limit on the user PATH variable in the registry
(`REG_SZ`). Expanding to `REG_EXPAND_SZ` allows longer values. The current
`Add-ToPathVariable` uses `[System.Environment]::SetEnvironmentVariable` which
writes `REG_SZ`. This should be changed to write `REG_EXPAND_SZ` and handle
expansion variables like `%USERPROFILE%` correctly.

### 6.4 Shell Restart vs Live Update

`SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0, "Environment")` notifies
running Explorer and most GUI applications to refresh their environment. However:
- `cmd.exe` sessions that were already open **do not** pick up the change
- PowerShell sessions that were already open **do not** pick up the change
- New shells opened after the alias is created **will** see it

This is identical behavior to winget's portable install. The module correctly
warns the user to restart their shell.

### 6.5 Cross-Drive Symlinks

Symlinks work across drives on NTFS. The `cmd /c mklink` approach handles
this correctly. However, if the links directory and the install directory are
on different drives, the symlink target must be an absolute path (which it always
is in this module).

### 6.6 Alias Name Collision

If two different exes are registered with the same alias name, the second
registration will overwrite the symlink of the first silently. This mirrors
winget's behavior. The `VerifyPackageAndSourceMatch` guard from winget should be
implemented to detect and warn about collisions.

### 6.7 UNC and Network Paths

Network paths are not supported as install locations. The symlink directory must
be on a local NTFS volume. The module does not validate this. `EnsureVolumeSupportsReparsePoints`
should be added as a preflight.

---

## 7. Registry Schema Reference

### 7.1 ARP Entry (per alias)

```
HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\Portable_{ProductCode}
│
├── (Default)                    [not set]
├── DisplayName                  REG_SZ   "My Tool"
├── DisplayVersion               REG_SZ   "1.0.0"
├── Publisher                    REG_SZ   "Vendor Name"
├── InstallLocation              REG_SZ   "C:\Users\...\WinGet\Packages\mytool"
├── PortableTargetFullPath       REG_SZ   "C:\Users\...\WinGet\Packages\mytool\mytool.exe"
├── PortableSymlinkFullPath      REG_SZ   "C:\Users\...\WinGet\Links\mytool.exe"
├── WinGetPackageIdentifier      REG_SZ   "MyVendor.MyTool"
├── WinGetSourceIdentifier       REG_SZ   "*DefaultSource"
├── InstallDirectoryCreated      REG_DWORD  0x1
├── InstallDirectoryAddedToPath  REG_DWORD  0x0   (1 if symlink failed)
├── SHA256                       REG_SZ   "abc123..."
├── UninstallString              REG_SZ   "powershell -Command ..."
├── NoModify                     REG_DWORD  0x1
└── NoRepair                     REG_DWORD  0x1
```

### 7.2 App Paths Entry (per alias)

```
HKCU\Software\Microsoft\Windows\CurrentVersion\App Paths\mytool.exe
│
├── (Default)   REG_SZ   "C:\Users\...\WinGet\Packages\mytool\mytool.exe"
└── Path        REG_SZ   "C:\Users\...\WinGet\Packages\mytool\"
```

### 7.3 PATH Entries Written

**User scope:**
```
HKCU\Environment
  Path  REG_EXPAND_SZ  ...;C:\Users\{user}\AppData\Local\Microsoft\WinGet\Links
```

**Machine scope:**
```
HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment
  Path  REG_EXPAND_SZ  ...;C:\Program Files\WinGet\Links
```

---

## 8. Path and Directory Reference

### 8.1 User Scope

| Purpose | Path |
|---|---|
| Portable install root | `%LOCALAPPDATA%\Microsoft\WinGet\Packages\` |
| Per-app install dir | `%LOCALAPPDATA%\Microsoft\WinGet\Packages\{ProductCode}\` |
| Links directory (symlinks) | `%LOCALAPPDATA%\Microsoft\WinGet\Links\` |
| ARP registry hive | `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\` |
| App Paths registry hive | `HKCU\Software\Microsoft\Windows\CurrentVersion\App Paths\` |
| PATH registry value | `HKCU\Environment\Path` |

### 8.2 Machine Scope (x64)

| Purpose | Path |
|---|---|
| Portable install root | `C:\Program Files\WinGet\Packages\` |
| Links directory | `C:\Program Files\WinGet\Links\` |
| ARP registry hive | `HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\` |
| PATH registry value | `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment\Path` |

### 8.3 Machine Scope (x86)

| Purpose | Path |
|---|---|
| Portable install root | `C:\Program Files (x86)\WinGet\Packages\` |
| Links directory | Same as x64: `C:\Program Files\WinGet\Links\` |

### 8.4 MSIX WindowsApps (reference only, cannot write to)

| Purpose | Path |
|---|---|
| Alias stubs | `%LOCALAPPDATA%\Microsoft\WindowsApps\` |
| Actual packages | `C:\Program Files\WindowsApps\{PackageFullName}\` |

---

## 9. Implementation Task Breakdown

### Phase 2 — Hardening and Feature Parity with Winget

- [ ] **2.1 Add reparse point support preflight check**
  - Port `EnsureVolumeSupportsReparsePoints` from `PortableFlow.cpp`
  - Check that `Get-PortableLinksLocation` volume supports reparse points
  - Abort with clear error if not (affects network drives, FAT32)
  - File: `AliasManager.psm1` — add as preflight in `New-AppAlias`

- [ ] **2.2 Add ProductCode collision guard**
  - Port `VerifyPackageAndSourceMatch` from `PortableFlow.cpp`
  - Before install: check if ARP key `Portable_{ProductCode}` already exists
  - If exists and registered to different exe, warn and require `-Force` to proceed
  - File: `AliasManager.psm1` — add `Test-ARPCollision` internal function

- [ ] **2.3 Add SHA256 tracking and verification**
  - Compute SHA256 of source exe before copy using `Get-FileHash`
  - Write to ARP entry `SHA256` value
  - On `Remove-AppAlias` and `Update-AppAlias`: verify hash before proceeding
  - Warn and require `-Force` if hash mismatch (file was modified)
  - File: `AliasManager.psm1` — add `Get-FileHash` call in `New-AppAlias`
    and verification in `Remove-AppAlias`

- [ ] **2.4 Fix PATH variable type to REG_EXPAND_SZ**
  - `[System.Environment]::SetEnvironmentVariable` writes `REG_SZ`
  - Change `Add-ToPathVariable` and `Remove-FromPathVariable` to use
    `reg.exe` or direct registry API to write `REG_EXPAND_SZ`
  - Correctly handle `%USERPROFILE%` and similar expand vars already in PATH
  - File: `AliasManager.psm1` — rewrite `Add-ToPathVariable` using `Microsoft.Win32.Registry`

- [ ] **2.5 Add elevation check for Machine scope**
  - Check `WindowsPrincipal.IsInRole(WindowsBuiltInRole.Administrator)` before
    machine-scope operations
  - Abort with clear message if not elevated
  - File: `AliasManager.psm1` — add `Assert-ElevationForScope` internal function

- [ ] **2.6 Add Developer Mode detection**
  - Check `HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock`
    `AllowDevelopmentWithoutDevLicense` before attempting symlink creation
  - If not set and not elevated, go directly to PATH fallback instead of
    attempting and failing `cmd /c mklink`
  - File: `AliasManager.psm1` — add `Test-SymlinkCapability` internal function

### Phase 3 — Update and Lifecycle

- [ ] **3.1 Implement `Update-AppAlias`**
  - Port upgrade ordering from `PortableInstaller::Install(OperationType::Upgrade)`:
    file ops FIRST, then ARP registration (reversed from fresh install)
  - Accept `-ExePath` for new binary, reuse existing `ProductCode`
  - Update SHA256 in ARP entry
  - File: `AliasManager.psm1` — add `Update-AppAlias` function

- [ ] **3.2 Implement `Test-AppAlias`**
  - Verify integrity of an existing alias
  - Check: exe exists, hash matches ARP, symlink points to correct target,
    links dir is on PATH
  - Return structured object with per-check results
  - File: `AliasManager.psm1` — add `Test-AppAlias` function

- [ ] **3.3 Implement `Repair-AppAlias`**
  - Re-run the symlink and PATH setup for an existing alias
  - Useful when symlink is broken (target moved) or PATH was modified externally
  - File: `AliasManager.psm1` — add `Repair-AppAlias` function

### Phase 4 — CLI Wrapper

- [ ] **4.1 Create `alias.ps1` CLI entry point**
  - Thin CLI wrapper around the module with subcommands:
    `alias add <exe> [--name <n>] [--scope user|machine]`
    `alias remove <name>`
    `alias list`
    `alias verify <name>`
    `alias repair <name>`
  - Follow winget's output style: status lines with `✓` and `✗` prefixes
  - File: `alias.ps1`

- [ ] **4.2 Add JSON output mode**
  - `--json` flag on all commands returns structured JSON instead of formatted text
  - Enables piping to agent tools and downstream processing
  - File: `alias.ps1`

- [ ] **4.3 Register the CLI itself as an alias**
  - After first use, offer to register `alias.ps1` as `alias` via `New-AppAlias`
  - Bootstraps the tool into the environment it manages
  - File: `alias.ps1` — add `-Bootstrap` flag

### Phase 5 — Archive and Multi-Exe Support

- [ ] **5.1 Add zip archive support**
  - Accept `-ArchivePath` pointing to a `.zip` containing multiple exes
  - Extract to install directory
  - For each exe in archive: create symlink with original filename or
    `PortableCommandAlias` equivalent (`-AliasMap` hashtable parameter)
  - Port `RecordToIndex = true` logic: track all file entries for clean uninstall
  - File: `AliasManager.psm1` — extend `New-AppAlias` or add `New-AppAliasFromArchive`

- [ ] **5.2 Implement tracking index (SQLite or JSON)**
  - For archive installs, write a tracking manifest at
    `{InstallDir}\{ProductCode}.json` listing all installed files and symlinks
  - Load on uninstall to remove all tracked entries
  - Mirrors winget's `PortableIndex` SQLite database
  - File: `AliasManager.psm1` — add `Write-PortableIndex`, `Read-PortableIndex`

- [ ] **5.3 Handle `ArchiveBinariesDependOnPath` equivalent**
  - Some archives have exes that depend on DLLs in the same directory
  - Add `-BinariesDependOnPath` switch: skip symlinks, add install dir to PATH directly
  - Mirrors winget's `ArchiveBinariesDependOnPath` manifest field
  - File: `AliasManager.psm1` — add conditional in `New-AppAlias` install flow

### Phase 6 — Native C++ Port (optional, for distribution)

- [ ] **6.1 Design C++ module structure**
  - Mirror `PortableInstaller.h` class layout
  - Extract: `AliasInstaller` class, `AliasFileEntry` struct, `AliasARPEntry` wrapper
  - Target: Win32 console app, no .NET dependency, ships as single exe

- [ ] **6.2 Implement Win32 symlink creation**
  - Use `CreateSymbolicLink(lpSymlinkFileName, lpTargetFileName, 0)` from `<winbase.h>`
  - Handle `ERROR_PRIVILEGE_NOT_HELD` and fall back to PATH modification
  - Link: `kernel32.lib`

- [ ] **6.3 Implement registry operations**
  - Use `RegOpenKeyExW`, `RegSetValueExW`, `RegDeleteKeyW` directly
  - Write PATH as `REG_EXPAND_SZ` from the start
  - Broadcast `WM_SETTINGCHANGE` via `SendMessageTimeoutW`

- [ ] **6.4 Implement SHA256 via Windows CNG API**
  - Use `BCryptCreateHash`, `BCryptHashData`, `BCryptFinishHash`
  - Link: `bcrypt.lib`
  - No third-party dependencies

---

## 10. Agent Instructions

This section tells an agent exactly how to work with this project.

### 10.1 Project State at Handoff

The file `AliasManager.psm1` is the current deliverable. It is a functional
PowerShell module that correctly:
- Creates a copy of any exe in the WinGet portable packages directory
- Creates a symlink in the WinGet links directory (with PATH fallback)
- Writes ARP and App Paths registry entries
- Manages the user/machine PATH variable
- Lists and removes aliases

It does NOT yet: verify SHA256 on removal, guard against ProductCode collisions,
check for reparse point support, handle archive inputs, or support upgrade.

### 10.2 How to Run the Current Module

```powershell
# Load
Import-Module .\AliasManager.psm1

# Create alias (User scope, Developer Mode required for symlink)
New-AppAlias -ExePath 'C:\Tools\rg.exe' -Alias 'rg' -Verbose

# List
Get-AppAlias

# Remove
Remove-AppAlias -ProductCode 'rg'

# Machine-wide (run PowerShell as Admin)
New-AppAlias -ExePath 'C:\Tools\fzf.exe' -Scope Machine
```

### 10.3 Key Reference for Any Agent Working on This

**The canonical source of truth for this project is:**
```
https://github.com/microsoft/winget-cli/blob/c8fe1ea0/src/AppInstallerCLICore/PortableInstaller.cpp
https://github.com/microsoft/winget-cli/blob/c8fe1ea0/src/AppInstallerCLICore/Workflows/PortableFlow.cpp
```

Read these files before making any behavioral changes. Every design decision in
`AliasManager.psm1` has a counterpart in these source files. The commit hash
`c8fe1ea0` is pinned — if you need to check a newer version of winget, compare
diff from this commit to avoid regressions.

**Do not use the GitHub raw URL directly** — fetch from the HTML view as shown
in the research session. The raw URL has access restrictions in the agent environment.

### 10.4 Invariants That Must Not Change

1. **ARP registration must happen before file operations for fresh installs.**
   If this order is reversed and the install fails midway, there is no recovery
   path. This is the same reason winget does it this way.

2. **`InstallDirectoryAddedToPath` in the ARP entry must be accurate.**
   The uninstall path uses this to decide whether to remove a symlink or a
   PATH entry. If this gets out of sync with reality, uninstall will fail silently.

3. **The links directory must always be on PATH after a successful install.**
   Even if no symlink was created (fallback case), the install directory itself
   must be on PATH. One of the two directories must be on PATH — never neither.

4. **`Remove-FromPathVariable` must check if the links dir is still in use**
   before removing it from PATH. If multiple aliases share the links directory,
   removing it during one alias's uninstall would break all others.

5. **`Resolve-AliasName` priority must remain: Alias → Rename → filename.**
   This matches the winget manifest `Commands[0]` → `--rename` → fallback order.
   Changing this breaks the contract with callers that rely on explicit naming.

### 10.5 Testing Checklist for Any Change

Before any change is considered done, verify:

- [ ] `New-AppAlias` on a real exe in User scope creates: install dir, copied exe,
      symlink in links dir, ARP key, App Paths key, links dir on PATH
- [ ] After `New-AppAlias`, opening a new shell and running the alias name
      resolves to the correct binary
- [ ] `Get-AppAlias` returns the alias with correct ProductCode, Alias, and Target
- [ ] `Remove-AppAlias` removes: symlink, copied exe, install dir (if empty),
      ARP key, App Paths key
- [ ] After `Remove-AppAlias`, the alias name no longer resolves in a new shell
- [ ] Running `New-AppAlias` twice with the same ProductCode does not create
      duplicate PATH entries
- [ ] Running `Remove-AppAlias` on a non-existent alias does not throw unhandled errors
- [ ] Machine scope operations succeed when running as Administrator
- [ ] Machine scope operations fail gracefully when not running as Administrator

### 10.6 File Conventions for Extensions

When adding new functions to `AliasManager.psm1`:

- Internal helper functions (not exported): use verb-noun format with no `Export-ModuleMember`
- All parameters that accept file paths: resolve to absolute paths immediately
  using `[System.IO.Path]::GetFullPath()`
- All registry paths: use the `HKCU:` or `HKLM:` PS drive prefix, not `Registry::`
- Code comments: only ASCII characters A-Z, a-z, 0-9, spaces. No punctuation.
  Separate distinct statements into separate comment lines.
- Error handling: use `try/catch` with `Write-Error` for recoverable errors and
  `throw` for unrecoverable ones. Never swallow exceptions silently.

### 10.7 Relationship to Broader Agent Toolchain

This module is intended to become part of the `.agents\bin\` toolchain. Once
Phase 4 (CLI wrapper) is complete:

```
%USERPROFILE%\.agents\bin\alias.exe   ← or alias.ps1
```

This should itself be registered as an alias using `New-AppAlias -Bootstrap`
so the tool manages its own presence in the environment. The `.agents` directory
structure uses NTFS junctions and environment variables to share tooling across
AI coding tools (opencode, Claude Code, Codex CLI, Copilot, Gemini CLI).

The alias system fills the gap of making any tool installed into `.agents\bin\`
resolvable by name without requiring manual PATH edits or MSIX packaging.

---

*Document generated from research session on `microsoft/winget-cli` commit `c8fe1ea0`.*
*All source references are pinned to this commit for reproducibility.*
