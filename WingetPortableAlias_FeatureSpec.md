# Feature Spec: Winget Portable Alias Lifecycle

**Source**: microsoft/winget-cli commit c8fe1ea0:
https://github.com/microsoft/winget-cli/blob/c8fe1ea0/src/AppInstallerCLICore/Workflows/PortableFlow.cpp;
https://github.com/microsoft/winget-cli/blob/c8fe1ea0/src/AppInstallerCLICore/PortableInstaller.cpp;
https://github.com/microsoft/winget-cli/blob/c8fe1ea0/src/AppInstallerCommonCore/Public/winget/PortableFileEntry.h;
https://github.com/microsoft/winget-cli/blob/c8fe1ea0/src/AppInstallerCommonCore/PortableARPEntry.cpp;
https://github.com/microsoft/winget-cli/blob/c8fe1ea0/src/AppInstallerCommonCore/PathVariable.cpp;
https://github.com/microsoft/winget-cli/blob/c8fe1ea0/src/AppInstallerSharedLib/Filesystem.cpp;
https://github.com/microsoft/winget-cli/blob/c8fe1ea0/src/AppInstallerCommonCore/Runtime.cpp;
https://github.com/microsoft/winget-cli/blob/c8fe1ea0/src/AppInstallerRepositoryCore/Microsoft/PortableIndex.cpp
**Feature boundary**: Portable install, upgrade, uninstall, alias, PATH, ARP, expected-state, and archive index behavior in `PortableFlow.cpp` lines 24-340 and `PortableInstaller.cpp` lines 27-507, with supporting value objects and helpers listed in Source.
**Language**: C++

---

## 1. Feature Summary

This feature installs, upgrades, and uninstalls portable package executables so they can be invoked through command aliases. It converts manifest, source, scope, installer, and command inputs into file entries, registry metadata, PATH updates, symlinks, and optional archive index records. It also verifies recorded state before mutating existing installs and cleans up fresh install failures.

---

## 2. Input Catalog

| ID | Name | Type | Source | Constraints | Required |
|----|------|------|--------|-------------|----------|
| I-01 | Package identifier | String | Manifest | Must be path-safe after sanitization with source identifier | Yes |
| I-02 | Package source identifier | String | Package version source or default source | Defaults to `*DefaultSource` when package version is absent | Yes |
| I-03 | Installer descriptor | Object | Selected installer manifest | Contains architecture, installer type, base installer type, commands, nested installer files, apps and features entries, and archive PATH dependency flag | Yes |
| I-04 | Installer payload path | File path or directory path | Installer download or archive extraction result | File path for single executable, directory path for archive extraction | Yes |
| I-05 | CLI rename argument | String | Command arguments | Empty allowed; non-empty value must remain unchanged after path-part sanitization | No |
| I-06 | CLI scope argument | Enum | Command arguments | User or Machine; if absent, user settings choose scope | No |
| I-07 | CLI install location | File path | Command arguments | Empty allowed; non-empty value overrides portable install root | No |
| I-08 | CLI force flag | Boolean | Command arguments | Allows package/source collision override and hash mismatch override | No |
| I-09 | CLI purge and preserve flags | Boolean | Command arguments | Affect uninstall directory removal policy | No |
| I-10 | User settings | Object | User settings store | Supplies install scope requirement, install scope preference, portable roots, and portable purge policy | Yes |
| I-11 | Known folders | File system paths | Operating system | LocalAppData, ProgramFiles, and ProgramFilesX86 must be resolvable for default roots | Yes |
| I-12 | Existing ARP state | Registry values | Uninstall registry under user or machine scope | May be absent; if present, contains metadata, target path, symlink path, hash, and state flags | No |
| I-13 | Existing portable index | SQLite database | Install location | May be absent; used for archive installs | No |
| I-14 | File system state | Paths, attributes, volume flags | Operating system | Includes reparse-point support, symlink status, existing files, directory emptiness, and volume identity | Yes |
| I-15 | Symlink creation result | Boolean or exception | Operating system | Privilege-not-held failure returns false; other failures propagate | Yes |
| I-16 | PATH registry value | String | User or machine environment registry | May contain expanded or unexpanded entries separated by semicolons | Yes |
| I-17 | Operation type | Enum | Workflow context | Install, upgrade, or uninstall | Yes |

---

## 3. Output Catalog

| ID | Name | Type | Condition | Consumer |
|----|------|------|-----------|----------|
| O-01 | Product code | String | During installer setup | ARP entry, install path, uninstall command, index filename |
| O-02 | Desired portable file entries | List of file, directory, and symlink descriptors | During install or upgrade | Installer execution layer |
| O-03 | Expected portable file entries | List of file and symlink descriptors | When existing ARP or index state exists | Verification and cleanup |
| O-04 | Installed files and directories | File system artifacts | On install or upgrade | User shell, future verification, uninstall |
| O-05 | Alias symlink | File system symlink | When symlink creation succeeds | Shell command resolution through PATH |
| O-06 | PATH registry update | Registry mutation | When alias links directory or install directory is not present in PATH | User or machine shell environment |
| O-07 | Environment change broadcast | OS notification | When PATH is changed | Explorer and future child processes |
| O-08 | ARP registry entry | Registry key and values | On install, upgrade, and state updates | Apps and Features, winget list, uninstall |
| O-09 | Portable index database | Hidden SQLite file | For archive installs | Upgrade and uninstall cleanup |
| O-10 | Verification result | Boolean | Before install, upgrade, or uninstall when expected state exists | Workflow gate |
| O-11 | Filesystem removals | Deleted files, symlinks, directories, and optional index | On uninstall or replacement | File system |
| O-12 | ARP registry deletion | Registry mutation | On uninstall completion | Apps and Features, winget list |
| O-13 | Operation return code | Integer | On install or uninstall workflow completion or handled exception | Caller |
| O-14 | User-facing messages | Text stream | On alias add, PATH change, failure cleanup, hash mismatch, remaining files | CLI reporter |
| O-15 | Error termination | Error code and report message | On invalid rename, unsupported reparse points, collision without force, hash mismatch without force, symlink path directory, or unhandled filesystem failure | Caller |

---

## 4. Process Map

### P-01: Validate Portable Preconditions

- **Trigger**: Portable install support check.
- **Logic**: Validate that rename argument is path-safe. Resolve target links directory for selected scope and require its volume to support reparse points.
- **Inputs**: I-05, I-06, I-10, I-14
- **Outputs**: O-15
- **Dependencies**: Path-part sanitizer, file system volume flags, workflow reporter.
- **Failure modes**: Invalid rename terminates with argument error. Missing reparse-point support terminates with portable reparse-point error.

### P-02: Build Portable Installer Context

- **Trigger**: Portable installer creation workflow.
- **Logic**: Resolve scope from argument or settings. Combine package identifier and source identifier into sanitized product code. Resolve default install root by scope and architecture unless install location argument overrides it. Capture manifest metadata for Apps and Features.
- **Inputs**: I-01, I-02, I-03, I-06, I-07, I-10, I-11
- **Outputs**: O-01, O-08
- **Dependencies**: User settings, known folders, manifest metadata.
- **Failure modes**: Unresolvable known folders or registry access failures propagate.

### P-03: Load Expected Installed State

- **Trigger**: Portable installer construction.
- **Logic**: Open existing ARP entry if present. Read display metadata, source identifiers, install location, target path, symlink path, hash, and state flags. If archive index exists, load all indexed entries; otherwise create expected entries from ARP target and symlink values.
- **Inputs**: I-12, I-13
- **Outputs**: O-03
- **Dependencies**: Registry access, portable index database.
- **Failure modes**: Missing ARP or index produces empty expected state. Corrupt index or registry read failure propagates.

### P-04: Guard Package And Source Collision

- **Trigger**: Existing ARP entry detected before install.
- **Logic**: Compare requested package identifier and source identifier with recorded values. Permit mismatch only when force flag is present.
- **Inputs**: I-01, I-02, I-08, I-12
- **Outputs**: O-15
- **Dependencies**: ARP state.
- **Failure modes**: Mismatch without force terminates with portable package already exists error.

### P-05: Build Desired File State

- **Trigger**: Portable install implementation.
- **Logic**: For an extracted archive directory, create desired entries for each top-level file or directory and create alias symlink entries for nested installer files. For a single executable, resolve command alias by filename, manifest command, and rename argument precedence, append `.exe` if needed, and create one file entry plus one symlink entry. Compute file hash for file entries.
- **Inputs**: I-03, I-04, I-05
- **Outputs**: O-02
- **Dependencies**: File system enumeration, hash calculation, extension appender.
- **Failure modes**: Missing payload, unreadable directory, or hash failure propagates.

### P-06: Verify Expected State

- **Trigger**: Before install, upgrade, or uninstall when expected entries exist.
- **Logic**: For expected file entries, compare recorded hash with current file hash when target exists. For expected symlink entries, verify existing symlink target against recorded target. Permit mismatch only when force flag is present.
- **Inputs**: I-08, I-14, I-12
- **Outputs**: O-10, O-15
- **Dependencies**: File hash calculation, symlink read and canonicalization.
- **Failure modes**: Hash or symlink mismatch without force terminates with portable uninstall failure error.

### P-07: Register ARP Metadata

- **Trigger**: Fresh install before file operations, upgrade after file operations, and state commits during install.
- **Logic**: Write package identifier, source identifier, uninstall command, installer type, display metadata, install date, URLs, install location, target path, symlink path, hash, and state flags to the scope and architecture registry location.
- **Inputs**: I-01, I-02, I-03, I-12, I-17
- **Outputs**: O-08
- **Dependencies**: Registry write access.
- **Failure modes**: Registry write failure propagates. Machine scope requires permission to write machine registry.

### P-08: Create Target Install Directory

- **Trigger**: Install or upgrade file execution.
- **Logic**: Create target install directory if absent. Record whether directory was created and record install location.
- **Inputs**: I-07, I-11, I-14
- **Outputs**: O-04, O-08
- **Dependencies**: File system create directories, registry write access.
- **Failure modes**: Directory creation or registry write failure propagates.

### P-09: Remove Previous Expected Entries

- **Trigger**: Desired state application.
- **Logic**: Remove existing indexed entries through the portable index when present; otherwise remove expected entries from ARP state. Delete index if it becomes empty. Remove previous install directory when target install location changes.
- **Inputs**: I-13, I-14, I-12
- **Outputs**: O-09, O-11
- **Dependencies**: Portable index database, file system removal.
- **Failure modes**: File removal, index update, or directory removal failure propagates.

### P-10: Install File And Directory Entries

- **Trigger**: Desired file or directory entry execution.
- **Logic**: For file entries, remove existing target, commit target path and hash when not indexed, then relocate source to target through rename with hard-link or copy fallback. For directory entries, rename when source and target are on the same volume and copy recursively when they are not.
- **Inputs**: I-04, I-14, I-12
- **Outputs**: O-04, O-08
- **Dependencies**: File system rename, hard-link support, copy operation, volume comparison.
- **Failure modes**: File or directory copy failure propagates.

### P-11: Install Alias Symlink Entry

- **Trigger**: Desired symlink entry execution.
- **Logic**: If archive binaries depend on PATH, skip symlink creation, add install directory to PATH, and record install-directory-on-PATH state. Otherwise, reject existing directories at the symlink path, record symlink path when not indexed, remove existing file at symlink path, then attempt symlink creation. If symlink creation fails due to privilege, add install directory to PATH and record install-directory-on-PATH state.
- **Inputs**: I-03, I-14, I-15, I-16
- **Outputs**: O-05, O-06, O-08, O-14, O-15
- **Dependencies**: Symlink creation API, PATH registry writer, file system remove.
- **Failure modes**: Existing directory at symlink path terminates with symlink path directory error. Non-privilege symlink errors propagate.

### P-12: Maintain PATH Environment

- **Trigger**: Install, symlink fallback, archive PATH dependency, or uninstall.
- **Logic**: Normalize PATH entry format. Append target path only when absent. Remove target path only when associated directory is absent or empty. Write PATH as expandable registry string and notify environment listeners.
- **Inputs**: I-16, I-14
- **Outputs**: O-06, O-07, O-14
- **Dependencies**: User or machine environment registry, broadcast notification.
- **Failure modes**: Registry permission failure propagates. Non-empty directory prevents PATH removal.

### P-13: Maintain Portable Index

- **Trigger**: Archive desired state application.
- **Logic**: Create or open hidden portable index database. Add or update desired file entries before installing them. Remove expected entries during cleanup. Delete index when it has no entries.
- **Inputs**: I-13, I-02, I-14
- **Outputs**: O-09
- **Dependencies**: SQLite storage, savepoints, file attributes.
- **Failure modes**: Unsupported index version, database write failure, or file attribute failure propagates.

### P-14: Uninstall Portable Package

- **Trigger**: Uninstall workflow after verification gate.
- **Logic**: Apply empty desired state to remove expected entries. Remove install directory if created and empty, or purge if requested. Remove shared links directory from PATH only when install directory was not added directly and directory is absent or empty. Delete ARP entry.
- **Inputs**: I-09, I-12, I-13, I-14, I-16
- **Outputs**: O-11, O-12, O-14
- **Dependencies**: File system removal, PATH registry writer, registry delete.
- **Failure modes**: Non-empty install directory is preserved and reported. Removal failures propagate.

### P-15: Clean Up Failed Fresh Install

- **Trigger**: Exception during fresh install.
- **Logic**: Treat desired entries as expected entries, clear desired entries, run uninstall cleanup, then terminate with portable uninstall failure.
- **Inputs**: I-17, O-02
- **Outputs**: O-11, O-12, O-13, O-14, O-15
- **Dependencies**: Uninstall process.
- **Failure modes**: Cleanup failure propagates through workflow exception handler.

### P-16: Report Operation Result

- **Trigger**: Install or uninstall workflow completion or handled exception.
- **Logic**: Add operation return code, emit accumulated messages, and publish correlated Apps and Features metadata after install.
- **Inputs**: I-03, I-17
- **Outputs**: O-10, O-13, O-14
- **Dependencies**: Workflow context and reporter.
- **Failure modes**: Reporter failure propagates through workflow handler.

---

## 5. Requirements Spec Sheet

### User Requirements

| ID | Requirement | Priority |
|----|-------------|----------|
| UR-01 | The user shall be able to install a portable executable so it is invokable by command alias from shell environments that use PATH. | Must |
| UR-02 | The user shall be able to install portable aliases in user scope or machine scope. | Must |
| UR-03 | The user shall be able to choose command alias names through manifest command metadata or rename input. | Must |
| UR-04 | The user shall be able to install archive-based portable packages with multiple files and aliases. | Should |
| UR-05 | The user shall be able to uninstall a portable package and remove its recorded files, aliases, PATH entries, and ARP entry. | Must |
| UR-06 | The system shall preserve unrelated files and aliases when uninstalling one portable package. | Must |
| UR-07 | The system shall detect changed installed files or alias targets before upgrade or uninstall. | Must |
| UR-08 | The user shall be able to override package/source collision and state mismatch checks with force input. | Should |
| UR-09 | The system shall expose portable package metadata to Apps and Features. | Should |

### Functional Requirements

| ID | Requirement | Satisfies |
|----|-------------|-----------|
| FR-01 | The feature shall derive product code from package identifier and source identifier, using default source when package version source is absent. | UR-01 |
| FR-02 | The feature shall reject rename input that changes after path-part sanitization. | UR-03 |
| FR-03 | The feature shall resolve install scope from scope argument, then scope requirement setting, then scope preference setting. | UR-02 |
| FR-04 | The feature shall map user-scope package root to LocalAppData under `Microsoft\WinGet\Packages`. | UR-02 |
| FR-05 | The feature shall map machine-scope x64 package root to ProgramFiles under `WinGet\Packages`. | UR-02 |
| FR-06 | The feature shall map machine-scope x86 package root to ProgramFilesX86 under `WinGet\Packages`. | UR-02 |
| FR-07 | The feature shall map user-scope links directory to LocalAppData under `Microsoft\WinGet\Links`. | UR-01 |
| FR-08 | The feature shall map machine-scope links directory to ProgramFiles under `WinGet\Links`. | UR-02 |
| FR-09 | The feature shall terminate portable install support when the links directory volume does not support reparse points. | UR-01 |
| FR-10 | The feature shall load existing ARP metadata and portable index entries before mutating existing installs. | UR-05 |
| FR-11 | The feature shall block installs over an existing product code with different package or source values unless force input is present. | UR-08 |
| FR-12 | The feature shall resolve single-executable alias name by filename, then manifest command override, then rename override, and shall ensure `.exe` extension. | UR-03 |
| FR-13 | The feature shall create one file entry and one symlink entry for a single executable install. | UR-01 |
| FR-14 | The feature shall create file and directory entries for archive payload contents. | UR-04 |
| FR-15 | The feature shall create archive alias entries from nested installer alias metadata or nested file names and shall ensure `.exe` extension. | UR-04 |
| FR-16 | The feature shall compute and record SHA256 for file entries. | UR-07 |
| FR-17 | The feature shall verify recorded file hashes and symlink targets before install, upgrade, or uninstall when expected state exists. | UR-07 |
| FR-18 | The feature shall require force input to proceed after expected-state mismatch. | UR-08 |
| FR-19 | The feature shall write ARP metadata before file operations for fresh installs. | UR-09 |
| FR-20 | The feature shall write ARP metadata after file operations for upgrades. | UR-09 |
| FR-21 | The feature shall create target install directory and record install location and directory-created state. | UR-01 |
| FR-22 | The feature shall remove an existing target file before installing a file entry. | UR-01 |
| FR-23 | The feature shall relocate file entries to target path using rename, hard-link fallback, or copy fallback. | UR-01 |
| FR-24 | The feature shall relocate directory entries by same-volume rename or cross-volume recursive copy. | UR-04 |
| FR-25 | The feature shall reject symlink creation when the alias path is an existing directory. | UR-01 |
| FR-26 | The feature shall remove an existing file at the alias path before creating the alias symlink. | UR-01 |
| FR-27 | The feature shall add install directory to PATH and record `InstallDirectoryAddedToPath` when symlink creation fails due to missing privilege. | UR-01 |
| FR-28 | The feature shall skip symlink creation, add install directory to PATH, and record `InstallDirectoryAddedToPath` when archive binaries depend on PATH. | UR-04 |
| FR-29 | The feature shall add the shared links directory to PATH after install when install directory was not added directly. | UR-01 |
| FR-30 | The feature shall avoid duplicate PATH entries when appending. | UR-06 |
| FR-31 | The feature shall remove PATH entries only when the associated directory is absent or empty. | UR-06 |
| FR-32 | The feature shall write PATH as an expandable string and broadcast environment change after PATH mutation. | UR-01 |
| FR-33 | The feature shall create or update a portable index for archive installs and store every desired entry in it. | UR-04 |
| FR-34 | The feature shall remove indexed entries during cleanup and delete the portable index when it is empty. | UR-05 |
| FR-35 | The feature shall remove installed target files, alias symlinks, fallback PATH entries, install directories, shared PATH entries, and ARP entry during uninstall according to recorded state. | UR-05 |
| FR-36 | The feature shall preserve non-empty install directories unless purge input is active. | UR-06 |
| FR-37 | The feature shall run cleanup after failed fresh installs using desired entries as expected entries. | UR-06 |
| FR-38 | The feature shall publish Apps and Features metadata with display name, version, publisher, installer type, and product code. | UR-09 |
| FR-39 | The feature shall write ARP value names `DisplayName`, `DisplayVersion`, `Publisher`, `InstallDate`, `URLInfoAbout`, `HelpLink`, `UninstallString`, `WinGetInstallerType`, `InstallLocation`, `TargetFullPath`, `SymlinkFullPath`, `SHA256`, `WinGetPackageIdentifier`, `WinGetSourceIdentifier`, `InstallDirectoryCreated`, and `InstallDirectoryAddedToPath`. | UR-09 |

### Non-Functional Requirements

| ID | Category | Requirement | Evidence |
|----|----------|-------------|---------|
| NFR-01 | Reliability | The feature shall order fresh install ARP registration before file operations so cleanup remains addressable after interrupted file operations. | P-07 |
| NFR-02 | Reliability | The feature shall order upgrade ARP registration after file operations so failed upgrades can be retried from prior metadata. | P-07 |
| NFR-03 | Reliability | The feature shall verify file hash and symlink target before mutating expected existing state. | P-06 |
| NFR-04 | Reliability | The feature shall avoid removing shared PATH entries while the shared directory contains files. | P-12, P-14 |
| NFR-05 | Compatibility | The feature shall require reparse-point-capable volumes for portable alias support. | P-01 |
| NFR-06 | Compatibility | The feature shall support user scope, machine x64 scope, and machine x86 install roots. | P-02 |
| NFR-07 | Compatibility | The feature shall use expandable PATH registry values and expand existing entries for comparison. | P-12 |
| NFR-08 | Reliability | The feature shall use database savepoints and interface locking when mutating archive index entries. | P-13 |
| NFR-09 | Observability | The feature shall report alias addition, PATH restart need, remaining install files, hash mismatch, and operation return codes. | P-16 |

---

## 6. Assumptions

| ID | Statement |
|----|-----------|
| ASS-01 | Boundary covers winget portable package alias behavior, not MSIX AppExecutionAlias behavior. |
| ASS-02 | Commit c8fe1ea0 is the source version for this spec because local dev docs pin that commit. |
| ASS-03 | Machine scope requires registry and file-system permissions for machine roots; the source propagates permission failures rather than modeling elevation as a feature step. |
| ASS-04 | Single-file portable install assumes the executable can run from the target install directory after relocation. |
| ASS-05 | Archive installs are the source-supported way to install executables with adjacent runtime dependencies. |
| ASS-06 | The feature's `force` input is a caller decision and is not derived from manifest data. |

---

## 7. Open Questions

| ID | Question |
|----|----------|
| OQ-01 | Should this project target winget parity for portable payloads only, or also support aliases to already installed applications whose executables must remain beside resource files? |
| OQ-02 | Should this project use winget's ARP value names `TargetFullPath` and `SymlinkFullPath`, or keep existing names `PortableTargetFullPath` and `PortableSymlinkFullPath` with a migration step? |
| OQ-03 | Should App Paths registration remain as a project extension outside winget parity? |
| OQ-04 | Should archive tracking use winget-compatible SQLite or a project-specific JSON index? |
| OQ-05 | Should single-executable install move, copy, hard-link, symlink, or wrap the original executable when the source is not a downloaded portable payload? |

---

## 8. Cleanroom Firewall Notice

This specification was produced by analysis of source code. It contains no
reproduced code. A second implementer working from this document alone,
without access to the original source, should be able to produce a
functionally equivalent feature.
