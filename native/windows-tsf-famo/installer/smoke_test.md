# Famo Windows local smoke test

This checklist validates the installer on the current development machine. It is not a cross-OS,
clean-machine, VM, or application-compatibility release matrix.

Artifact under test: `installer\dist\Famo-Setup-<version>.exe`

## Required local checks

1. Build the installer once from the current Stable native output.
2. Install or repair it with one UAC approval.
3. Run `Test-FamoHealth.ps1 -Json` and `Test-FamoTsfRegistration.ps1 -Json`.
4. Reproduce the original long-input Explorer scenario in the applications already installed on this
   computer. Explorer must keep one responsive process and record no hang event.
5. Kill or stall the runtime once and verify the first key fails open within 50 ms and later keys do
   not wait for IPC.
6. Type in Notepad plus one already-installed Chromium/Electron or Office host. Do not install an app
   only to add a matrix row.
7. Verify `%AppData%\Rime` is unchanged and product data stays under `%LOCALAPPDATA%\Famo`.
8. Uninstall or roll back only when that lifecycle is part of the current change.

`smoke-harness.ps1` may automate the local install/health/uninstall path. Manual observations stay
manual; missing optional compatibility checks do not turn the local gate into `BLOCKED`.

## Optional release certification

Windows-version, clean-machine, mixed-DPI, secure-desktop, and broad host coverage require separate
user approval after stating download size, disk use, expected duration, UAC count, and cleanup. Do not
download an ISO, create a VM, install extra applications, or retain evidence bundles by default.
