# Requirements Document: App Alias Rebuild From Winget Portable Spec

## Overview

This plan converts winget portable alias behavior into work items for this project. It also records gaps surfaced by the VS Code Insiders alias test: copying one executable out of an installed application directory can break applications that load resources beside the executable. Winget handles portable payloads by relocating the payload into its install directory, and handles dependency archives by installing the archive contents together.

## User Roles

- CLI user: creates, lists, verifies, repairs, updates, and removes aliases.
- Automation caller: invokes alias operations from scripts and needs JSON or structured output.
- Maintainer: updates module behavior against winget portable semantics.

## Requirements

### Requirement 1: Portable Payload Alias

**User Story:** As a CLI user, I want to install a portable payload as a command alias, so that new shells can run it by name.

**Acceptance Criteria:**
1. WHEN user installs a single portable executable THEN system SHALL create a product directory under the scoped portable package root.
2. WHEN user installs a single portable executable THEN system SHALL create a file entry and an alias entry using filename, command metadata, or rename input.
3. WHEN alias name lacks `.exe` THEN system SHALL append `.exe`.
4. WHEN symlink creation succeeds THEN system SHALL place the symlink in the scoped links directory.
5. WHEN symlink creation fails due to missing privilege THEN system SHALL add the install directory to PATH and record `InstallDirectoryAddedToPath`.
6. WHEN install succeeds and install directory was not added to PATH THEN system SHALL add the scoped links directory to PATH.

**Edge Cases:**
- Existing alias path is a directory: fail before writing symlink.
- Existing alias path is a file: remove and replace it.
- PATH already contains target directory: do not duplicate entry.

### Requirement 2: External Application Alias

**User Story:** As a CLI user, I want to alias an already installed application, so that the alias does not relocate an executable that depends on adjacent files.

**Acceptance Criteria:**
1. WHEN user creates an alias in external-target mode THEN system SHALL leave source executable in its original directory.
2. WHEN source executable is under an application install directory THEN system SHALL make command execution resolve to that original executable path.
3. WHEN target application requires working directory or adjacent resources THEN system SHALL launch with the original executable directory as process context.
4. WHEN uninstalling an external-target alias THEN system SHALL remove alias artifacts without deleting the original application executable.

**Edge Cases:**
- Electron or Chromium application: command must not execute a copied exe from a package-only directory.
- Source executable removed after alias creation: verify command reports missing target.
- Source path contains spaces: alias creation and launch preserve path.

### Requirement 3: Scope And Roots

**User Story:** As a CLI user, I want aliases in user scope or machine scope, so that commands resolve at the chosen visibility.

**Acceptance Criteria:**
1. WHEN scope is User THEN system SHALL use LocalAppData `Microsoft\WinGet\Packages` and `Microsoft\WinGet\Links`.
2. WHEN scope is Machine and architecture is x64 THEN system SHALL use ProgramFiles `WinGet\Packages` and `WinGet\Links`.
3. WHEN scope is Machine and architecture is x86 THEN system SHALL use ProgramFilesX86 `WinGet\Packages` and ProgramFiles `WinGet\Links`.
4. WHEN Machine scope is requested without required permission THEN system SHALL fail before partial writes.

**Edge Cases:**
- Custom install root setting exists: system uses configured root.
- Links directory volume lacks reparse-point support: system fails before install.

### Requirement 4: ARP State

**User Story:** As a maintainer, I want registry state to match winget portable semantics, so that install, verify, update, and uninstall can use recorded state.

**Acceptance Criteria:**
1. WHEN installing THEN system SHALL write `DisplayName`, `DisplayVersion`, `Publisher`, `InstallDate`, `URLInfoAbout`, `HelpLink`, `UninstallString`, `WinGetInstallerType`, `InstallLocation`, `TargetFullPath`, `SymlinkFullPath`, `SHA256`, `WinGetPackageIdentifier`, `WinGetSourceIdentifier`, `InstallDirectoryCreated`, and `InstallDirectoryAddedToPath`.
2. WHEN a previous project registry schema exists THEN system SHALL migrate or read both old and winget value names.
3. WHEN fresh install begins THEN system SHALL create ARP metadata before file operations.
4. WHEN update begins THEN system SHALL write ARP metadata after file operations.
5. WHEN uninstall completes THEN system SHALL delete the ARP entry.

**Edge Cases:**
- Existing ProductCode belongs to another package/source: require force.
- Registry write fails: fail operation and run cleanup for fresh install.

### Requirement 5: Verification And Repair

**User Story:** As an automation caller, I want structured verification results, so that scripts can detect broken aliases.

**Acceptance Criteria:**
1. WHEN verifying an alias THEN system SHALL check target existence, hash match, symlink existence, symlink target, PATH entry, App Paths entry if enabled, and ARP state.
2. WHEN verification finds mismatched hash or symlink target THEN system SHALL report failed check and require force for destructive operations.
3. WHEN repair is requested THEN system SHALL recreate missing symlink or PATH entry from recorded state.
4. WHEN source target is missing THEN repair SHALL fail without fabricating a target.

**Edge Cases:**
- PATH value has environment variables: comparison uses expanded value.
- Links directory contains other aliases: verify does not mark shared directory invalid.

### Requirement 6: Archive Payload Support

**User Story:** As a CLI user, I want to install archive payloads with multiple files, so that executables with adjacent dependencies run after install.

**Acceptance Criteria:**
1. WHEN payload is an extracted directory THEN system SHALL install file and directory entries under the product install directory.
2. WHEN nested alias metadata exists THEN system SHALL create alias entries for nested executables.
3. WHEN `BinariesDependOnPath` is set THEN system SHALL skip symlink creation and add install directory to PATH.
4. WHEN installing archive payload THEN system SHALL record every installed entry in a tracking index.
5. WHEN uninstalling archive payload THEN system SHALL use tracking index for cleanup.

**Edge Cases:**
- Source and target directories are on different volumes: system copies directory recursively.
- Index exists and becomes empty: system deletes index.

## Non-Functional Requirements

- **Reliability:** WHEN fresh install fails THEN system SHALL attempt cleanup from desired entries.
- **Reliability:** WHEN uninstalling one alias THEN system SHALL not remove shared PATH links directory if it contains files.
- **Compatibility:** WHEN writing PATH THEN system SHALL use `REG_EXPAND_SZ`.
- **Compatibility:** WHEN PATH changes THEN system SHALL broadcast environment change.
- **Observability:** WHEN commands run with JSON output flag THEN system SHALL return structured result objects with operation, alias, scope, target, changed paths, and check results.

## Out of Scope

- MSIX AppExecutionAlias reparse stubs.
- Windows Settings App execution aliases toggle.
- Store package identity.
- Kernel reparse tag creation.

## Build Sequence

1. Add tests for current failure: aliasing VS Code Insiders must not run copied Electron executable.
2. Add registry schema adapter for winget value names and prior project value names.
3. Add preflight functions: scope permission, reparse-point support, symlink capability, ProductCode collision.
4. Split install into portable payload mode and external-target mode.
5. Add SHA256 recording and expected-state verification.
6. Add `Test-AppAlias`, then `Repair-AppAlias`, then `Update-AppAlias`.
7. Add PATH writer using expandable registry string and environment broadcast.
8. Add archive payload support and tracking index.
9. Add CLI wrapper with structured output.
10. Update `AppAliasGenerator_DevDocs.md` to reference this spec and align registry names.

## Open Questions

- Should default mode be portable payload mode or external-target mode?
- Should archive index be winget-compatible SQLite or JSON for this project?
- Should App Paths remain enabled by default?
- Should existing aliases created with old registry value names be migrated on list, verify, or install?
