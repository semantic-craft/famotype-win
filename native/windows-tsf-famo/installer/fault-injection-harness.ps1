<#
.SYNOPSIS
  Drive the installer's /FamoFail= injection points and assert that each one
  rolls the machine back to the exact previous Bridge projection.
.DESCRIPTION
  The .iss has carried deterministic injection points since the transactional
  rewrite, but nothing ever executed them: /FamoFail= appears nowhere outside
  famo-setup.iss and one source-text contract test, and the acceptance evidence
  under dist\acceptance-evidence was produced by hand. This harness is the
  missing driver.

  It runs strictly against the machine it is invoked on. Every step is followed
  by a health assertion, and the run aborts on the first unhealthy state rather
  than stacking a second failure on top of a bad rollback.

  Requires an elevated session. It does not self-elevate: the caller opens one
  administrator window, exactly as smoke-harness.ps1 requires.
.NOTES
  Emits one JSON object to -ResultPath. Exit 0 only if every selected step met
  its expectation and the machine finished Ready.
#>
[CmdletBinding()]
param(
  [string] $Installer = '',
  # Injection points reachable on an install/upgrade run. Uninstall-anchored
  # points need an uninstall cycle and are out of scope here.
  [string[]] $Phases = @(
    'after-prepare',
    'after-verify',
    'after-switch',
    'after-user-state',
    'after-active-verify',
    'ready-debt-before-ready',
    'ready-after-phase-before-seedcommit'
  ),
  [string] $ResultPath = '',
  [switch] $SkipMigration,
  [switch] $DryRun
)

$ErrorActionPreference = 'Stop'
$InstallerDir = $PSScriptRoot
$NativeDir = Split-Path -Parent $InstallerDir
$BrandKey = 'HKLM:\SOFTWARE\Famo\InputMethod'

if (-not $ResultPath) {
  $ResultPath = Join-Path $InstallerDir 'dist\fault-injection-result.json'
}

function Need([string] $Path, [string] $Hint) {
  if (-not (Test-Path -LiteralPath $Path)) { throw "缺失：$Path`n$Hint" }
}

# The projection a rollback has to restore byte for byte. FileId is included
# because a path and hash can match while the file was replaced underneath.
function Get-Projection {
  if (-not (Test-Path -LiteralPath $BrandKey)) {
    return [pscustomobject]@{ Present = $false }
  }
  $brand = Get-ItemProperty -LiteralPath $BrandKey
  $bridge = [string]$brand.BridgePath
  $fileId = ''
  $bridgeFileHash = ''
  if ($bridge -and (Test-Path -LiteralPath $bridge)) {
    $bridgeFileHash = (Get-FileHash -LiteralPath $bridge -Algorithm SHA256).Hash
    $fileId = ((& fsutil.exe file queryfileid $bridge 2>&1) -join ' ').Trim()
  }
  [pscustomobject]@{
    Present        = $true
    InstallState   = [string]$brand.InstallState
    InstallDir     = [string]$brand.InstallDir
    ActiveVersion  = [string]$brand.ActiveVersion
    BridgePath     = $bridge
    BridgeAbi      = [string]$brand.BridgeAbi
    BridgeHash     = [string]$brand.BridgeHash
    BridgeFileHash = $bridgeFileHash
    BridgeFileId   = $fileId
    TransactionId  = [string]$brand.TransactionId
  }
}

# --is-input-tip: 0 present, 1 absent, 2 probe failed.
function Get-TipState([string] $installDir) {
  $settings = Join-Path $installDir 'settings\FamoSettings.exe'
  if (-not (Test-Path -LiteralPath $settings)) { return 'no-settings-exe' }
  & $settings --is-input-tip | Out-Null
  switch ($LASTEXITCODE) {
    0 { 'present' } 1 { 'absent' } default { "probe-failed($LASTEXITCODE)" }
  }
}

# What a rollback owes: the artifact projection, byte for byte. InstallState is
# deliberately excluded -- 'RolledBack' is one of the four terminal states and
# is the correct outcome of a compensated transaction, not drift. FileId is in
# the set because a path and hash can match while the file was replaced.
function Compare-Projection($expected, $actual) {
  $drift = @()
  foreach ($p in @('InstallDir', 'ActiveVersion', 'BridgePath',
                   'BridgeAbi', 'BridgeHash', 'BridgeFileHash', 'BridgeFileId')) {
    if ($expected.$p -cne $actual.$p) {
      $drift += "${p}: expected='$($expected.$p)' actual='$($actual.$p)'"
    }
  }
  $drift
}

function Invoke-Installer([string] $exe, [string] $failPhase, [string] $logPath) {
  $arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-',
                 "/LOG=$logPath")
  if ($failPhase) { $arguments += "/FamoFail=$failPhase" }
  $run = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -Wait
  $run.ExitCode
}

if (-not $Installer) {
  $Installer = Get-ChildItem -LiteralPath (Join-Path $InstallerDir 'dist') `
    -Filter 'Famo-Setup-*.exe' -File -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1 -ExpandProperty FullName
}
Need $Installer '先构建安装器，或用 -Installer 指定。'

$baseline = Get-Projection
if (-not $baseline.Present) { throw '没有已安装的 Famo 投影可作为回滚基线。' }
if ($baseline.InstallState -ne 'Ready') {
  throw "基线状态不是 Ready（$($baseline.InstallState)）；先把机器恢复到 Ready 再跑。"
}
$baselineTip = Get-TipState $baseline.InstallDir

Write-Host '== Famo fault-injection harness ==' -ForegroundColor Cyan
Write-Host "Installer : $Installer"
Write-Host "Baseline  : ABI=$($baseline.BridgeAbi) state=$($baseline.InstallState) tip=$baselineTip"
Write-Host "Phases    : $($Phases -join ', ')"

if ($DryRun) {
  Write-Host 'DRY RUN PASS: inputs resolved, baseline healthy, machine untouched.' -ForegroundColor Green
  exit 0
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw '请在一个管理员 PowerShell 窗口中运行本脚本；脚本不会自行触发 UAC。'
}

$logDir = Join-Path $InstallerDir 'dist\fault-logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$steps = @()
$aborted = $null

foreach ($phase in $Phases) {
  Write-Host "-- inject $phase" -ForegroundColor Yellow
  $log = Join-Path $logDir "fail-$phase.log"
  $exit = Invoke-Installer $Installer $phase $log
  $after = Get-Projection
  $tip = if ($after.Present) { Get-TipState $after.InstallDir } else { 'no-install' }
  $drift = Compare-Projection $baseline $after

  # A rejected transaction must fail loudly, land on the RolledBack terminal
  # state, and leave the artifact projection untouched. A zero exit would mean
  # the injection point never fired.
  $ok = ($exit -ne 0) -and ($drift.Count -eq 0) -and ($tip -eq $baselineTip) -and
        ($after.InstallState -eq 'RolledBack')
  $steps += [pscustomobject]@{
    Step = "inject:$phase"; Expect = 'fail + RolledBack + exact artifact rollback'
    ExitCode = $exit; InstallState = $after.InstallState; Drift = $drift
    Tip = $tip; Passed = $ok; Log = $log
  }
  if (-not $ok) {
    $aborted = "回滚未还原到基线：$phase"
    Write-Host "   FAIL exit=$exit state=$($after.InstallState) drift=$($drift -join '; ') tip=$tip" -ForegroundColor Red
    break
  }
  Write-Host "   ok (exit=$exit, state=RolledBack, artifacts unchanged, tip=$tip)" -ForegroundColor Green

  # Every later phase must start from Ready, or it is testing a different
  # transition than the one it names. Recovery is a full installer run, and it
  # lands on PendingReboot whenever the Bridge is loaded -- which cannot be
  # cleared without a logon. So a multi-phase sweep is only unattended on a
  # machine where the Bridge is not loaded; otherwise stop and say so.
  if ($phase -ne $Phases[-1]) {
    $recoverLog = Join-Path $logDir "recover-after-$phase.log"
    $null = Invoke-Installer $Installer '' $recoverLog
    $recovered = Get-Projection
    if ($recovered.InstallState -ne 'Ready') {
      $aborted = "注入 $phase 后无法在不重启的情况下回到 Ready" +
                 "（现为 $($recovered.InstallState)）。Bridge 被宿主进程加载时," +
                 "多阶段扫描必须在 Bridge 未加载的机器上跑，或每阶段之间重启。"
      Write-Host "   STOP: $aborted" -ForegroundColor Yellow
      break
    }
    $baseline = $recovered
  }
}

if (-not $aborted -and -not $SkipMigration) {
  Write-Host '-- clean migration (no injection)' -ForegroundColor Yellow
  $log = Join-Path $logDir 'migrate.log'
  $exit = Invoke-Installer $Installer '' $log
  $after = Get-Projection
  $tip = Get-TipState $after.InstallDir
  $v3Intact = ($baseline.BridgeFileHash -eq
    (Get-FileHash -LiteralPath $baseline.BridgePath -Algorithm SHA256 -ErrorAction SilentlyContinue).Hash)
  $ok = ($exit -eq 0) -and ($after.InstallState -eq 'Ready') -and
        ($after.BridgeAbi -ne $baseline.BridgeAbi) -and ($tip -eq 'present') -and $v3Intact
  $steps += [pscustomobject]@{
    Step = 'migrate'; Expect = 'Ready on new ABI, previous bridge bytes intact'
    ExitCode = $exit; FromAbi = $baseline.BridgeAbi; ToAbi = $after.BridgeAbi
    InstallState = $after.InstallState; Tip = $tip; PreviousBridgeIntact = $v3Intact
    Passed = $ok; Log = $log
  }
  if (-not $ok) { $aborted = '迁移未达成 Ready/新 ABI/旧工件完好' }
  else {
    Write-Host "   ok (ABI $($baseline.BridgeAbi) -> $($after.BridgeAbi), tip=$tip)" -ForegroundColor Green

    Write-Host '-- repair idempotence (same installer again)' -ForegroundColor Yellow
    $before = Get-Projection
    $log = Join-Path $logDir 'repair.log'
    $exit = Invoke-Installer $Installer '' $log
    $now = Get-Projection
    # A repair may mint a new runtime target; the Bridge must not move.
    $bridgeDrift = @('BridgePath', 'BridgeAbi', 'BridgeHash', 'BridgeFileHash',
                     'BridgeFileId') |
      Where-Object { $before.$_ -cne $now.$_ }
    $ok = ($exit -eq 0) -and ($now.InstallState -eq 'Ready') -and
          ($bridgeDrift.Count -eq 0)
    $steps += [pscustomobject]@{
      Step = 'repair'; Expect = 'Ready, bridge untouched'
      ExitCode = $exit; BridgeDrift = $bridgeDrift
      InstallState = $now.InstallState; Passed = $ok; Log = $log
    }
    if (-not $ok) { $aborted = 'repair 非幂等或触碰了 Bridge' }
    else { Write-Host '   ok (bridge untouched)' -ForegroundColor Green }
  }
}

$final = Get-Projection
$finalTip = if ($final.Present) { Get-TipState $final.InstallDir } else { 'no-install' }
$healthy = $final.Present -and ($final.InstallState -eq 'Ready') -and
           ($finalTip -eq 'present')

$result = [pscustomobject]@{
  installer   = $Installer
  installerSha256 = (Get-FileHash -LiteralPath $Installer -Algorithm SHA256).Hash
  baseline    = $baseline
  baselineTip = $baselineTip
  steps       = $steps
  final       = $final
  finalTip    = $finalTip
  machineHealthy = $healthy
  aborted     = $aborted
  passed      = (-not $aborted) -and $healthy -and
                (($steps | Where-Object { -not $_.Passed }).Count -eq 0)
}
New-Item -ItemType Directory -Force -Path (Split-Path $ResultPath -Parent) | Out-Null
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $ResultPath -Encoding UTF8
Write-Host "result -> $ResultPath"

if (-not $healthy) {
  Write-Host "MACHINE NOT HEALTHY: state=$($final.InstallState) tip=$finalTip" -ForegroundColor Red
  Write-Host '恢复：用上一版安装器重跑一次修复安装。' -ForegroundColor Red
}
if ($result.passed) {
  Write-Host 'FAULT INJECTION PASS' -ForegroundColor Green
  exit 0
}
Write-Host "FAULT INJECTION FAIL: $aborted" -ForegroundColor Red
exit 1
