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

The Bridge owns only TSF/COM orchestration, edit sessions, the UIElement facade,
the bounded Runtime pipe client, and fail-open behavior. Engine work, config,
deployment, rendering, and product features belong in Runtime. Any change that
can alter `FamoTextService.dll` must explain why it cannot live in Runtime and
must either reuse the current frozen artifact or explicitly bump the Bridge ABI.

Bridge ABI 3 is an exceptional Bridge release because candidate UI delivery is
owned by the in-process bounded pipe client. It replaces the one-shot UI
connection with a reconnecting, latest-state worker while leaving the primary
key path and Runtime protocol range unchanged. This lifecycle cannot be moved
to Runtime: after the optional UI pipe misses startup or disconnects, only the
Bridge still owns the queued TSF state that must be replayed to a replacement
Runtime.

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
replies using the client's negotiated wire version.

Compatibility for the first migration is:

| Bridge | Runtime v2 | Runtime v3 |
| --- | --- | --- |
| Legacy Bridge, wire v2 | yes | yes |
| Stable Bridge ABI 1, extended Hello | no | yes |

The installer lays down and verifies Runtime v3 before it registers Stable
Bridge ABI 1, so it never activates the new Bridge against Runtime v2.
Subsequent Runtime releases must continue to accept the protocol range frozen
in the Bridge artifact. Before a future protocol removal:

1. release a Runtime that accepts both the old and new Bridge;
2. confirm that Runtime is broadly installed;
3. publish a new Bridge ABI at a new `bridge\vN` path;
4. allow that exceptional migration to use PendingReboot if the old Bridge is
   loaded.

## Acceptance boundary

Automated gates prove artifact separation, protocol compatibility, native
tests, payload contracts, and installer compilation. A release is not accepted
as reboot-free until Windows system testing records both:

- one migration from the legacy version-directory DLL to
  `bridge\v1` (this migration may require one final reboot);
- at least two consecutive Runtime-only upgrades where the Bridge path and
  SHA-256 remain unchanged, `PendingReboot` is never entered, and typing works
  in existing and newly opened applications.
