# Famo Runtime Boundary Audit

This is the #32 closeout audit for the Windows IME runtime. The goal is to
prove that support and smoke tests can diagnose TSF registration, runtime
readiness, IPC, engine/session state, deployer maintenance, settings UI, and
candidate/status UI independently.

Famo borrows discipline from Mozc, Fcitx, IBus, and windows-chewing-tsf:
bounded probes, explicit service state, restartable runtime assumptions,
renderer/UIElement contracts, and read-only support diagnostics. Famo does not
import their full daemon or plugin framework into the Windows release.

## Boundary Matrix

| Runtime surface | Owner | Health state | Recovery path | Existing check |
| --- | --- | --- | --- | --- |
| TSF registration and profile visibility | Installer registration plus `FamoTextService.dll` identity | Registered, missing, wrong identity, hidden from current-user TIP list | Repair install, unregister/reinstall, rerun TSF audit before UI debugging | `Test-FamoTsfRegistration.ps1 -Json`, smoke S01/S02 |
| Runtime process startup | `FamoRuntime.exe` launched by installer/HKLM Run and smoke recovery | `NotInstalled`, `InstalledStopped`, `WrongIdentity`, `Ready`, `Degraded` | Start runtime, killed-runtime `RECOV`, reboot `REBOOT`, reinstall only after identity failures | `Test-FamoHealth.ps1 -Json`, `smoke-harness.ps1` READY/RECOV/REBOOT |
| Pipe readiness and IPC hot path | Runtime named pipe plus Weasel IPC client | `Ready`, `Hung`, `Broken`, bounded IPC timeout/error | Return failure to caller, keep typing thread bounded, relaunch runtime if health confirms stopped/hung | `Test-FamoHealth.ps1` H6, `bounded-ipc-connect.patch`, `BoundedIpcPatchContractTests`, diagnostics `ipcPipeConnectMs` |
| Engine/session ABI | `FamoRimeEngine.dll` behind `famo_engine_api.h` and active Weasel sessions | ABI active, session unavailable, maintenance disabled, degraded backend | Fail open while maintenance runs, recreate/reselect sessions after runtime/engine recovery | `FamoEngineApiContractTests`, `Test-FamoHealth.ps1` H9, `MaintenanceSafeTypingContractTests` |
| Deployer and maintenance queue | Settings `DeployService` plus `FamoDeploy.exe` | `Idle`, `Pending`, `Running`, `Succeeded`, `Failed`, `RetryAvailable` | Queue/coalesce reloads, show retryable failure, keep settings UI responsive, do not block typing | `DeployServiceTests`, `SettingsReloadStatusContractTests`, `ApplyFeedbackContractTests`, smoke `MAINT`, diagnostics `deployQueue` |
| Settings UI apply surface | WinUI pages and `App.ReportReloadResult` | Current reload token, pending/running/succeeded/failed status | Surface honest status and retry affordance without freezing the page | `SettingsReloadStatusContractTests`, `ApplyFeedbackContractTests` |
| Candidate/status UI and TSF UIElement | Weasel candidate renderer, Famo status bar/popup, `ITfCandidateListUIElement` facade | Visible, hidden, empty, lost focus, lost capture, stale-state risk | Hide or clear mirrors when composition/session/runtime state disappears; record panel failure separately from runtime/deploy | `PANEL-SMOOTHNESS.md`, `PanelSmoothnessContractTests`, smoke `PANEL`, health `panelFailureBoundary = SeparateFromRuntimeAndDeploy` |
| Safe diagnostics and local timing | `Get-FamoDiagnostics.ps1` plus `FamoTimingLog` | Read-only bundle, opt-in timing, bounded/rate-limited log | Collect local support evidence without reading clipboard, typed text, dictionaries, secrets, or user Weasel files | `DiagnosticsAndTimingContractTests`, diagnostics `healthProbeMs`/`ipcPipeConnectMs`/`deployQueue`/`candidateStatusUi` |

## Deployer Failure Boundary

A failing deployer must not block settings UI or typing.

- `DeployService` resolves the deployer, queues work, publishes
  `DeployQueueSnapshot`, and records `DeployQueueStatus.Failed` with
  `RetryAvailable` for UI recovery.
- Settings pages call `App.ReportReloadResult(...)` and subscribe to
  `DeployService.QueueChanged`; they display pending/running/succeeded/failed
  state instead of waiting synchronously for the deployer.
- The typing hot path is covered separately by bounded IPC and
  maintenance-safe typing checks. Smoke row `MAINT` is the user-visible proof
  that deploy/maintenance while composing does not freeze the foreground app or
  leave candidate/preedit UI stale.
- Diagnostics report deploy timing as `deployQueue`, separate from
  `runtimeHealth`, `ipc`, and `candidateStatusUi`.

## Candidate And Status UI Boundary

Candidate/status stale-state checks can run without rebuilding the engine.

- `PANEL-SMOOTHNESS.md` owns renderer behavior: no focus steal, first-frame DPI,
  empty/focus/input-method/maintenance/killed-runtime/lost-capture cleanup, and
  TSF UIElement metadata.
- The `PANEL` smoke row is intentionally a GUI observation row, not an engine
  deploy or schema rebuild row. It can be run after S03-S06/MAINT against the
  installed runtime and current session state.
- `Test-FamoHealth.ps1 -Json` reports `panelProbe` and
  `panelFailureBoundary = SeparateFromRuntimeAndDeploy`; a stale panel is not
  automatically an H6 pipe failure or a deploy queue failure.
- `Get-FamoDiagnostics.ps1 -Json` carries the same separation under
  `timings.candidateStatusUi` and `concerns.candidateStatusUi`.

## Non-Goals From Fcitx And IBus

Famo should keep the useful engineering discipline from Fcitx/IBus without
turning the Windows product into their framework.

- Non-goal: a generic Fcitx-style addon/plugin host for arbitrary frontends.
- Non-goal: a D-Bus-style IBus daemon/engine/panel protocol on Windows.
- Non-goal: moving TSF profile ownership into a separate panel process.
- Non-goal: a cross-platform runtime framework abstraction before the Windows
  TSF/runtime/deployer/panel boundaries are stable.
- Non-goal: reporting every UI problem as a runtime restart problem; panel,
  runtime, deployer, and diagnostics remain separate concerns.

## Closeout Rule

Before the focused current-machine smoke is considered complete, the following
must all be true for the affected path:

1. TSF registration audit passes or clearly reports the missing registration
   boundary.
2. Health audit reports a concrete `healthState` and bounded IPC result.
3. Deploy queue failures are visible and retryable without blocking settings.
4. Only the affected `MAINT`, `PANEL`, `DIAG`, or zero-write checks have observable results.
5. Diagnostics distinguish `ipc`, `runtimeHealth`, `deployQueue`, and
   `candidateStatusUi`.
