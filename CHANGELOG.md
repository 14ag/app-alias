# Changelog

This project uses the Keep a Changelog structure.

## [Unreleased]

### Added

- Native C++ core for MSIX app execution alias creation, listing, verification, and removal.
- CLI commands for `create`, `list`, `remove`, and `verify`.
- Packaged proxy launcher that forwards arguments to the configured target executable.
- Target icon extraction into MSIX visual assets.
- Build, preflight, and certificate helper scripts.
- Repository documentation for setup, architecture, and troubleshooting.
- Documented CLI JSON fields and exit codes.

### Changed

- Native MSIX packages are the only supported path for Settings-visible aliases.
- Package identity sanitization now strips punctuation from alias stems to avoid AppModel activation failures.
- Proxy binaries build with static CRT.
- CLI non-JSON list output is tab-delimited.
- `create --force` refuses foreign aliases before deployment.

### Removed

- PowerShell alias prototype, batch wrapper, and old winget research notes.
