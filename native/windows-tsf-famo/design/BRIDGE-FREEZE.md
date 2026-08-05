# Stable TSF Bridge release and freeze contract

Famo Windows uses two independently released native layers:

- **Stable Bridge**: the in-process `FamoTextService.dll` loaded by
  `ctfmon.exe` and applications. Its registered path is
  `%ProgramFiles%\Famo\bridge\v<bridge-abi>\FamoTextService.dll`.
- **Runtime payload**: `FamoRuntime.exe`, the engine DLLs, profile tool,
  settings application, and data. Every release is installed into a new
  immutable `%ProgramFiles%\Famo\versions\<version>-<manifest>-<transaction>`
  directory.

Routine releases must reuse the exact signed Stable Bridge artifact. When the
Bridge ABI and SHA-256 match the registered Bridge, the installer:

1. does not extract or overwrite the Bridge;
2. does not probe whether the Bridge is loaded;
3. does not switch away from the profile;
4. does not unregister or re-register TSF;
5. stops the old Runtime, commits the new version directory, and starts the new
   Runtime.

This is the no-reboot path. A Bridge ABI or signed-byte change is deliberately
a different operation and retains the existing PendingReboot recovery path.

## Responsibility whitelist

The Bridge owns TSF/COM orchestration, edit sessions, the UIElement facade, the
bounded Runtime pipe client, fail-open behavior, and the host-owned presentation
adapter described below. Engine work, dictionaries, deployment, persisted
state, and product policy remain in Runtime. Candidate layout, painting, UIA,
and light-dismiss behavior have one shared implementation; ABI 6 links that
implementation into the Bridge only because the presented HWND must live in the
host process. Any change that can alter `FamoTextService.dll` must explain why
it cannot live in Runtime and must either reuse the current frozen artifact or
explicitly bump the Bridge ABI.

Bridge ABI 3 is an exceptional Bridge release because candidate UI delivery is
owned by the in-process bounded pipe client. It replaces the one-shot UI
connection with a reconnecting, latest-state worker while leaving the primary
key path and Runtime protocol range unchanged. This lifecycle cannot be moved
to Runtime: after the optional UI pipe misses startup or disconnects, only the
Bridge still owns the queued TSF state that must be replayed to a replacement
Runtime.

Bridge ABI 4 is the frozen UI-less-candidate release. The in-process TSF
facade implements host ownership through
`ITfIntegratableCandidateListUIElement`; this must remain in the TIP because
only the Bridge participates in the host's TSF UI-element negotiation. ABI 4
continues to use Runtime protocol v3. Its bytes and artifact directory are
immutable after publication.

Bridge ABI 5 is the frozen Windows Search and caret-geometry release. It adds
two contracts
that cannot live wholly in Runtime:

- `ITfFunctionProvider` / `ITfFnSearchCandidateProvider`, which Windows Search
  discovers on the in-process TIP, with engine-backed candidates carried over
  the new stateless Runtime protocol v4 command;
- caret-range collapse plus host-DPI coordinate normalization, which requires
  the Bridge-owned `ITfContextView`, view HWND, and edit-session range.

Its bytes and `bridge\v5` artifact directory are immutable after publication.

Bridge ABI 6 is the next exceptional Bridge release. It adds one contract that
cannot live wholly in Runtime:

- the self-drawn candidate HWND and presentation adapter. Windows Search and
  other composition-sensitive hosts require that popup to be created in the
  host process and owned by the exact HWND returned from
  `ITfContextView::GetWnd`. Runtime continues to own engine state and receives
  UI state with self-drawing disabled, so it never creates a duplicate popup.

The ABI 6 presenter must preserve the host's `BeginUIElement`/`Show` decision,
must suppress itself when `GetWnd` is unavailable or the keyboard-disabled
security compartment is active, and must destroy/recreate the popup when the
exact owner changes. It must never force `pbShow`, use private window
bands/UIAccess, or record composition text for diagnostics.

ABI 6 is installed only at `bridge\v6`; it never replaces ABI 3, ABI 4, or ABI
5 bytes. It becomes a frozen production artifact only after the signed DLL is
used to create and verify a new manifest. Until then, development artifacts
remain unsigned evidence and are not release artifacts.

Bridge ABI 13 is the sandboxed-host release. ABI 7 through 12 were local
diagnosis iterations and were never published. It carries three contracts that
only the Bridge can hold:

- activation acquires every interface a callback can need — the keystroke
  manager, `ITfUIElementMgr`, `ITfSourceSingle` — and starts the session worker
  before it advises the first sink. Chromium, Electron and SearchHost text
  stores deliver `ITfThreadMgrEventSink::OnSetFocus` synchronously from inside
  `AdviseSink`, and a context created in that window keeps whatever the service
  held at that instant for its whole life. Acquiring the UI element manager
  afterwards leaves that context permanently unable to begin a candidate
  element: keys and composition still work and only the candidate window is
  missing.
- the host composition is maintained whether or not inline preedit is switched
  on, and the host selection during composition is the caret rather than the
  converted segment. A text store can only measure text it holds, so this is
  what makes `GetTextExt` answer with a usable rectangle.
- candidate appearance arrives over the authenticated pipe as part of the
  published snapshot. The presenter must never open the style file itself: it
  draws inside the host process, and a sandboxed host such as SearchHost cannot
  read the user profile at all, so a self-reading presenter silently falls back
  to the built-in skin in exactly the hosts that need it most.

ABI 13 is installed only at `bridge\v13` and never replaces earlier bytes.

## Artifact rules

`installer/build-bridge-artifact.ps1` creates:

- `FamoTextService.dll`
- `bridge-manifest.txt` with identity, Bridge ABI, supported Runtime protocol
  range, size, and SHA-256

Create the artifact only after Authenticode signing. An existing artifact
directory refuses different DLL bytes. Never rebuild a Bridge under an
existing ABI and never derive the Bridge from the current Runtime build during
routine packaging.

The signed artifact should live in immutable release storage. Restore it to an
ignored local directory and pass that directory explicitly:

```powershell
pwsh -File installer/build-installer.ps1 `
  -NativeOutput <runtime-output> `
  -BridgeArtifact <frozen-signed-bridge-artifact> `
  -AppVersion <version>
```

`build-installer.ps1` rejects missing, malformed, hash-mismatched, or unsigned
Bridge artifacts for a production build.

## Protocol compatibility

Runtime protocol v3 adds an extended `Hello` carrying the Bridge ABI and a
protocol range. Runtime v3 still accepts legacy v2 empty `Hello` frames and
replies using the client's negotiated wire version. Runtime protocol v4 adds
the stateless `SearchCandidates` command; v2/v3 peers reject that command while
their existing session commands remain compatible. Runtime protocol v5 adds
`GetStyleOverlay`, which is how candidate appearance reaches a sandboxed host.

Current compatibility is:

| Bridge | Runtime v2 | Runtime v3 | Runtime v4 | Runtime v5 |
| --- | --- | --- | --- | --- |
| Legacy Bridge, wire v2 | yes | yes | yes | yes |
| Bridge ABI 3/4, wire through v3 | no | yes | yes | yes |
| Bridge ABI 5/6, wire v4 | no | no | yes | yes |
| Bridge ABI 13, wire v5 | no | no | no | yes |

A Bridge stamps its own `kProtocolVersion` on the very first `Hello` frame, and
a Runtime rejects any frame above the version it was compiled with before
negotiation is ever reached. Pairing a Bridge with an older Runtime therefore
does not degrade — every host in the session fails to connect at all, including
plain unsandboxed ones. Packaging must take the Runtime from the same source
revision as the Bridge; a stale build directory is enough to ship a product
where nothing types.

The installer lays down and verifies the matching Runtime before it registers
a new Bridge, so it never activates ABI 6 against a pre-v4 Runtime. Subsequent
Runtime releases must continue to accept the protocol range frozen in each
Bridge artifact. Before a future protocol removal:

1. release a Runtime that accepts both the old and new Bridge;
2. confirm that Runtime is broadly installed;
3. publish a new Bridge ABI at a new `bridge\vN` path;
4. allow that exceptional migration to use PendingReboot if the old Bridge is
   loaded.

## Acceptance boundary

Automated gates prove artifact separation, protocol compatibility, native
tests, payload contracts, and installer compilation. A release is not accepted
as reboot-free until Windows system testing records both:

- one migration from the previously frozen Bridge to the new `bridge\vN`
  directory (this migration may require one final reboot when the old Bridge
  is loaded);
- at least two consecutive Runtime-only upgrades where the Bridge path and
  SHA-256 remain unchanged, `PendingReboot` is never entered, and typing works
  in existing and newly opened applications.
