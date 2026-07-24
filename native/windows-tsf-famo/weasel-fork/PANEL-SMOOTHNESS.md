# Famo Panel Smoothness Audit

This is the #29 contract for the visible IME surfaces: candidate window,
preedit/auxiliary panel, floating status bar, quick popup panel, and the TSF
UIElement-facing candidate-list facade.

The goal is to keep the native Windows renderer and its accessibility boundary
explicit without depending on source snapshots from another repository.

## Source Map

| Surface | Source | Contract |
| --- | --- | --- |
| Candidate window | `runtime-protocol/src/candidate_window.cpp` | Retained non-activating layered window, per-snapshot DPI, bounded surface/resource reuse, duplicate/anchor/selection fast paths, and failure-to-hide. |
| TSF candidate UIElement | `text-service/src/candidate_ui_element.{h,cpp}` | Native `ITfCandidateListUIElement` count, selection, strings, paging, show/UI-less request and lifecycle notifications from the same Composition. |
| Floating status bar | `overlay/include/FamoStatusBar.h`, `overlay/WeaselUI/FamoStatusBar.cpp`, `features/status-bar.patch` | `WS_EX_NOACTIVATE`, `MA_NOACTIVATE`, DPI/layout before `ShowWindow(SW_SHOWNOACTIVATE)`, hide on focus-out, capture cleanup on drag interruption. |
| Quick popup panel | `overlay/include/FamoPopupPanel.h`, `overlay/WeaselUI/FamoPopupPanel.cpp` | `WS_EX_NOACTIVATE`, `MA_NOACTIVATE`, DPI/layout before first visible show, `SetCapture` for outside-click close, Esc hook only while open, lost capture starts close. |
| Runtime/deploy boundary | `tests/Test-FamoHealth.ps1`, `installer/smoke-harness.ps1`, settings `DeployService` | Runtime health, deploy queue, and panel manual probe are separate facts. A panel stale/failure observation must not be reported as a runtime or deployer failure. |

## No-Focus-Steal Contract

1. Candidate window and floating panels must use no-activate window traits or
   no-activate show/move calls.
2. Mouse activation handlers must return `MA_NOACTIVATE` where the surface is
   clickable.
3. Manual smoke must keep the foreground app active while clicking/dragging the
   status bar and opening/closing the popup.

Current evidence:

- The runtime candidate window uses `WS_EX_NOACTIVATE`, returns
  `MA_NOACTIVATE`, and uses `SWP_NOACTIVATE` for both full submit and
  anchor-only movement.
- Famo status bar and popup use `WS_EX_NOACTIVATE`,
  `ShowWindow(SW_SHOWNOACTIVATE)`, and `SetWindowPos(... SWP_NOACTIVATE ...)`.

## First-Frame DPI Contract

Before any candidate/status/popup surface becomes visible or moves on a monitor:

1. Resolve the target monitor from the input/caret/anchor rect.
2. Read effective DPI for that monitor.
3. Recompute scaled layout rectangles and text resources.
4. Move/show with no activation.

Current evidence:

- The runtime Snapshot carries target-monitor caret, work area and DPI before
  layout or the anchor-only fast path runs.
- Famo status bar calls `_CurrentDpi()`, `_LayoutButtons()`,
  `SetWindowPos(... SWP_NOACTIVATE ...)`, then `ShowWindow(SW_SHOWNOACTIVATE)`.
- Famo popup calls `_CurrentDpi()`, `_Layout()`,
  `SetWindowPos(... SWP_NOACTIVATE ...)`, then `ShowWindow(SW_SHOWNOACTIVATE)`.

## Stale-State Contract

Visible panels are mirrors of engine/session/runtime state. They must clear or
hide when the mirrored state disappears.

| Stale trigger | Required behavior | Current anchor |
| --- | --- | --- |
| Empty context or not composing | Hide candidate UI. | `CandidateWindow::ShouldShow` rejects empty or unavailable snapshots. |
| Empty candidate list | Candidate UIElement exposes count `0`; visible renderer hides. | `CandidateUiElement::Update` clears candidates and ends the UIElement. |
| Focus leave or input-method switch | Hide candidate/status surfaces. | Weasel `FocusOut` hides UI; Famo status-bar patch calls `HideBar()`. |
| Deploy/maintenance while composing | Clear current sessions and hide preedit/candidate state while disabled. | `engine-abi.patch` calls `m_ui->Hide()` during maintenance and reports disabled status. |
| Killed runtime | Typing path fails open; health reports runtime stopped/hung/broken separately. | `Test-FamoHealth.ps1` `healthState` and smoke `RECOV`. |
| Lost mouse capture | Clear hover/press/drag state and close transient popup. | `FamoStatusBarInteractionModel.CaptureChanged`, status bar `OnCaptureChanged`, popup `OnCaptureChanged`. |

## TSF UIElement Audit

Famo owns a native `CandidateUiElement` beside the self-drawn popup. This TSF
object is the canonical accessibility and UI-less compatibility surface. An
extra focusable UI Automation provider is intentionally not added to the popup:
doing so would duplicate candidate state and weaken the no-activate contract.

| UIElement facet | Status | Evidence / gap |
| --- | --- | --- |
| Count | Supported | `GetCount` reads the immutable candidate vector. |
| Selection | Supported | `GetSelection` reads the clamped highlighted index. |
| Strings | Supported for candidate text | `GetString` returns the candidate text. Labels/comments remain renderer metadata so an accessibility query cannot change commit semantics. |
| Page index | Partial compatibility | `GetPageIndex`, `SetPageIndex` and `GetCurrentPage` use validated page starts; the current engine snapshot normally publishes one page. |
| Show mode | Supported | `Show` / `IsShown` maintain TSF-visible state without activating the popup. |
| UI-less mode | Supported as TSF show request | `BeginUIElement` records the manager's `show_allowed` decision; the popup reads the same decision from UiState. |
| Update notifications | Supported | Later snapshots call `UpdateUIElement` with count/selection/string/page flags. |
| End lifecycle | Supported | Empty candidates and teardown call `EndUIElement`. |

## System Visual Preferences

- High Contrast is read from `SPI_GETHIGHCONTRAST`; the renderer switches to
  system window/text/highlight pairs, removes the shadow and uses an opaque
  panel with a visible border.
- The popup has no candidate animation or material blur, so disabling animation
  or transparency needs no alternate timing path.
- The candidate thread pumps `WM_SETTINGCHANGE` / `WM_THEMECHANGED` and
  invalidates the current frame. A system preference change therefore repaints
  the latest Snapshot without waiting for another key.

## Diagnostic Boundary

The panel layer must be diagnosable without conflating it with the runtime or
deployer:

1. `Test-FamoHealth.ps1 -Json` reports runtime/install state through
   `healthState` and includes separate `panelProbe` and
   `panelFailureBoundary = SeparateFromRuntimeAndDeploy` markers.
2. Settings deploy/reload state is owned by `DeployService` queue snapshots.
3. Smoke row `PANEL` records the only facts that currently need a real GUI:
   no focus steal, mixed-DPI first frame, focus/input-method switch cleanup,
   killed-runtime cleanup, lost-capture cleanup, and TSF UIElement audit.

Until a real automated window probe exists, a panel failure is a `PANEL` smoke
failure, not an `H6` runtime pipe failure and not a deploy queue failure.

## Future Work

1. If we replace or heavily rewrite the renderer, borrow Mozc's renderer shape:
   dedicated layout manager, DPI-dependent resources, hide on uninitialized or
   empty data, and no-activate show/move.
2. If TSF UIElement fidelity becomes a user-visible problem, replace the current
   single-page facade with a stable candidate model snapshot like
   windows-chewing-tsf.
3. If panel stale state becomes frequent, add an automated local window probe
   under diagnostics instead of expanding the health script into GUI automation.
