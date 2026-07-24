#requires -version 5
<#
.SYNOPSIS
  Applies the ordered Famo feature patch chain to a clean Weasel checkout.

.DESCRIPTION
  This script owns source-level feature changes. Branding/GUID replacement is
  handled separately by apply-famo-identity.ps1.

  Patch order:
    1. bounded-ipc-connect.patch
    2. instant-apply.patch
    3. launch-settings.patch
    4. ensure-deployed.patch
    5. select-schema.patch
    6. auto-pair.patch
    7. cjk-spacing.patch

  Run order: features first, then apply-famo-statusbar.ps1 if needed, then
  apply-famo-identity.ps1. Patches are generated against the pinned checkout
  described in SOURCE.md.

.PARAMETER UpstreamDir
  Path to the rime/weasel checkout.

.PARAMETER DryRun
  Applies the dependent patch chain in a temporary copy and deletes it.
#>
param(
  [Parameter(Mandatory = $true)] [string] $UpstreamDir,
  [switch] $DryRun
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

# Ordered feature patch chain, relative to this script.
# select-schema depends on the IPC scaffold from instant-apply.
$patches = @(
  'features/bounded-ipc-connect.patch',
  'features/instant-apply.patch',
  'features/launch-settings.patch',
  'features/ensure-deployed.patch',
  'features/select-schema.patch',
  'features/auto-pair.patch',
  'features/cjk-spacing.patch'
)

if (-not (Test-Path $UpstreamDir)) { throw "UpstreamDir missing: $UpstreamDir" }
if (-not (Test-Path (Join-Path $UpstreamDir '.git'))) {
  throw "UpstreamDir is not a git repository: $UpstreamDir"
}

Write-Output "=== Famo features patches  (DryRun=$($DryRun.IsPresent))  upstream=$UpstreamDir ==="

# The patches touch overlapping files, so per-patch reverse checks are not a
# stable idempotency signal after later patches adjust nearby context. Use a
# final sentinel instead: if cjk-spacing's famo_cjk_number_spacing marker exists,
# the whole feature chain is considered applied. DryRun uses a temp copy so the
# caller's checkout never needs rollback.
$patchUpstreamDir = $UpstreamDir
$dryRunRoot = $null
if ($DryRun.IsPresent) {
  $dryRunRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("famo-features-dryrun-{0}" -f [guid]::NewGuid().ToString('N'))
  $patchUpstreamDir = Join-Path $dryRunRoot 'upstream'
  New-Item -ItemType Directory -Force -Path $dryRunRoot | Out-Null
  Copy-Item -LiteralPath $UpstreamDir -Destination $patchUpstreamDir -Recurse -Force
}

$sentinelFile = Join-Path $patchUpstreamDir 'include/WeaselIPCData.h'
$alreadyApplied = (Test-Path $sentinelFile) -and
                  (Select-String -Path $sentinelFile -Pattern 'famo_cjk_number_spacing' -SimpleMatch -Quiet)

Push-Location $patchUpstreamDir
try {
  if ($alreadyApplied) {
    Write-Output "  [SKIP] famo_cjk_number_spacing already exists in include/WeaselIPCData.h."
  }
  else {
    foreach ($rel in $patches) {
      $patch = Join-Path $here $rel
      if (-not (Test-Path $patch)) { throw "Patch missing: $patch" }
      Write-Output "  patch: $rel"

      & git apply --check --whitespace=nowarn -- $patch 2>$null
      if ($LASTEXITCODE -ne 0) {
        throw "Patch does not apply cleanly: $rel. Start from a fresh pinned checkout; run features before apply-famo-statusbar."
      }
      & git apply --whitespace=nowarn -- $patch
      if ($LASTEXITCODE -ne 0) { throw "git apply failed (exit=$LASTEXITCODE): $rel" }
      Write-Output "    $(if ($DryRun) { '[would apply]' } else { '[applied]' }) $rel"
    }
  }
} finally {
  Pop-Location
  if ($dryRunRoot -and (Test-Path -LiteralPath $dryRunRoot)) {
    $resolvedDryRunRoot = (Resolve-Path -LiteralPath $dryRunRoot).Path
    $tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
    $leaf = Split-Path -Leaf $resolvedDryRunRoot
    if ($resolvedDryRunRoot.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
        $leaf.StartsWith('famo-features-dryrun-', [System.StringComparison]::OrdinalIgnoreCase)) {
      Remove-Item -LiteralPath $resolvedDryRunRoot -Recurse -Force
    } else {
      Write-Warning "Skipped DryRun temp cleanup; path failed safety check: $resolvedDryRunRoot"
    }
  }
}

Write-Output "=== Done. Next: run apply-famo-statusbar.ps1 if needed, then apply-famo-identity.ps1, then build per BUILD-NOTES.md. ==="
