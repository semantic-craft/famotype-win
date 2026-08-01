<#
.SYNOPSIS
  Drive the installer's /FamoFail= injection points for upgrade, fresh-install,
  or uninstall recovery.
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
  Flushes one JSONL record per completed step to -StepResultPath, then writes a
  summary object to -ResultPath. Exit 0 only if every selected step met its
  expectation and the machine finished in the mode's healthy state.
#>
[CmdletBinding()]
param(
  [string] $Installer = '',
  # Injection points reachable on an install/upgrade run. -Uninstall replaces
  # this default with the five uninstall recovery points unless explicitly set.
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
  [string] $StepResultPath = '',
  [string] $HealthScript = '',
  [switch] $SkipMigration,
  # Inject during a fresh install instead of an upgrade. A RolledBack tombstone
  # is allowed after each failure, but no Ready projection or input TIP is.
  [switch] $CleanInstall,
  # Inject the five uninstall persistence boundaries. Each failed uninstall is
  # re-entered with a retained copy of its own uninstaller, verified as clean
  # NotInstalled, then reinstalled before the next point.
  [switch] $Uninstall,
  [switch] $DryRun
)

$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) {
  throw '必须用 PowerShell 7 运行。'
}
if (-not [Environment]::Is64BitProcess) {
  throw '必须用 64 位 PowerShell 运行；32 位 registry view 会漏读 HKLM64 安装状态。'
}
$InstallerDir = $PSScriptRoot
$NativeDir = Split-Path -Parent $InstallerDir
$BrandKey = 'HKLM:\SOFTWARE\Famo\InputMethod'
$UninstallDeleteAnchorKey = 'HKLM:\SOFTWARE\Famo\UninstallRecovery'
$UninstallKey =
  'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{F0A12B34-FAA0-4F0E-9C7A-FAMO00010000}_is1'
$UninstallPhases = @(
  'uninstall-intent-before-commit',
  'uninstall-intent-after-commit',
  'uninstall-delete-anchor-before-commit',
  'uninstall-delete-anchor-after-commit',
  'uninstall-brand-deleted-before-anchor-retire'
)

if ($CleanInstall -and $Uninstall) {
  throw '-CleanInstall 与 -Uninstall 不能同时使用。'
}
if ($Uninstall) {
  if (-not $PSBoundParameters.ContainsKey('Phases')) {
    $Phases = $UninstallPhases
  }
  $unknownPhases = @($Phases | Where-Object { $_ -notin $UninstallPhases })
  if ($unknownPhases.Count -gt 0) {
    throw "未知 uninstall 注入点：$($unknownPhases -join ', ')"
  }
}
if (@($Phases).Count -eq 0) { throw '至少指定一个注入点。' }
$invalidPhaseNames = @($Phases | Where-Object { $_ -notmatch '^[a-z0-9-]+$' })
if ($invalidPhaseNames.Count -gt 0) {
  throw "非法注入点名称：$($invalidPhaseNames -join ', ')"
}

if (-not $ResultPath) {
  $ResultPath = Join-Path $InstallerDir 'dist\fault-injection-result.json'
}
if (-not $StepResultPath) {
  $StepResultPath = [IO.Path]::ChangeExtension($ResultPath, '.steps.jsonl')
}
if (-not $HealthScript) {
  $HealthScript = Join-Path $NativeDir `
    'weasel-fork\tests\Test-FamoHealth.ps1'
}
if ([string]::Equals(
    [IO.Path]::GetFullPath($ResultPath),
    [IO.Path]::GetFullPath($StepResultPath),
    [StringComparison]::OrdinalIgnoreCase)) {
  throw '-ResultPath 与 -StepResultPath 必须不同。'
}

function Need([string] $Path, [string] $Hint) {
  if (-not (Test-Path -LiteralPath $Path)) { throw "缺失：$Path`n$Hint" }
}

function Assert-NoReparsePath([string] $Path) {
  $item = Get-Item -LiteralPath $Path
  while ($null -ne $item) {
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
      throw "拒绝 reparse 路径：$($item.FullName)"
    }
    $item = $item.Parent
  }
}

function Test-InjectionLog([string] $Path, [string] $Phase) {
  if (-not (Test-Path -LiteralPath $Path)) { return $false }
  $text = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop
  $text.IndexOf(
    "injected transaction failure: $Phase",
    [StringComparison]::Ordinal) -ge 0
}

function Invoke-HealthAudit([string] $ExpectedState) {
  $hostExe = (Get-Process -Id $PID).Path
  $start = [Diagnostics.ProcessStartInfo]::new()
  $start.FileName = $hostExe
  $start.UseShellExecute = $false
  $start.CreateNoWindow = $true
  $start.RedirectStandardOutput = $true
  $start.RedirectStandardError = $true
  foreach ($argument in @(
      '-NoLogo', '-NoProfile', '-NonInteractive',
      '-File', $HealthScript, '-Json')) {
    $start.ArgumentList.Add($argument)
  }
  $process = [Diagnostics.Process]::new()
  $process.StartInfo = $start
  try {
    $null = $process.Start()
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    $text = $stdout.GetAwaiter().GetResult()
    $errorText = $stderr.GetAwaiter().GetResult()
    $exitCode = $process.ExitCode
  } finally {
    $process.Dispose()
  }
  try {
    $checks = @($text | ConvertFrom-Json -ErrorAction Stop)
    $states = @($checks | Select-Object -ExpandProperty healthState -Unique)
    $failed = @($checks | Where-Object { -not $_.pass } |
      Select-Object -ExpandProperty id)
    $actualState = if ($states.Count -eq 1) { [string]$states[0] } else {
      $states -join ','
    }
    [pscustomobject]@{
      Passed = ($exitCode -eq 0) -and ($failed.Count -eq 0) -and
        ($states.Count -eq 1) -and
        [string]::Equals(
          $actualState, $ExpectedState,
          [StringComparison]::Ordinal)
      ExpectedState = $ExpectedState
      ActualState = $actualState
      ExitCode = $exitCode
      FailedChecks = $failed
    }
  } catch {
    [pscustomobject]@{
      Passed = $false
      ExpectedState = $ExpectedState
      ActualState = 'audit-error'
      ExitCode = $exitCode
      FailedChecks = @()
      Error = "$($_.Exception.Message); stderr=$errorText"
    }
  }
}

function Initialize-StepResultFile {
  $parent = Split-Path $StepResultPath -Parent
  if ($parent) {
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    Assert-NoReparsePath $parent
  }
  $stream = [IO.File]::Open(
    $StepResultPath, [IO.FileMode]::Create, [IO.FileAccess]::Write,
    [IO.FileShare]::Read)
  try { $stream.Flush($true) } finally { $stream.Dispose() }
}

function Write-StepResult($Step) {
  $line = ($Step | ConvertTo-Json -Compress -Depth 8) +
    [Environment]::NewLine
  $bytes = [Text.UTF8Encoding]::new($false).GetBytes($line)
  $stream = [IO.File]::Open(
    $StepResultPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::Write,
    [IO.FileShare]::Read)
  try {
    $null = $stream.Seek(0, [IO.SeekOrigin]::End)
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush($true)
  } finally {
    $stream.Dispose()
  }
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
  $probe = Start-Process -FilePath $settings -ArgumentList '--is-input-tip' `
    -PassThru -Wait
  switch ($probe.ExitCode) {
    0 { 'present' } 1 { 'absent' }
    default { "probe-failed($($probe.ExitCode))" }
  }
}

function Get-UninstallerPath {
  Need $UninstallKey '先安装当前 RC，使 Inno uninstall 注册项存在。'
  $command = [string](Get-ItemPropertyValue -LiteralPath $UninstallKey `
    -Name UninstallString)
  $trimmed = $command.Trim()
  if ($trimmed -match '^"([^"]+\.exe)"(?:\s|$)') {
    $path = $Matches[1]
  } elseif ($trimmed.EndsWith('.exe', [StringComparison]::OrdinalIgnoreCase)) {
    $path = $trimmed
  } else {
    throw "无法安全解析 UninstallString：$command"
  }
  $expectedRoot = [IO.Path]::GetFullPath(
    (Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'Famo')).TrimEnd('\')
  $actualRoot = [IO.Path]::GetFullPath(
    (Split-Path -Parent $path)).TrimEnd('\')
  $leaf = Split-Path -Leaf $path
  if (-not [string]::Equals(
      $actualRoot, $expectedRoot,
      [StringComparison]::OrdinalIgnoreCase) -or
      $leaf -notmatch '^unins\d{3}\.exe$') {
    throw "拒绝固定安装根以外的 uninstaller：$path"
  }
  Need $path 'uninstaller 文件已缺失，不能开始新的 uninstall 注入点。'
  $rootItem = Get-Item -LiteralPath $actualRoot
  $exeItem = Get-Item -LiteralPath $path
  if (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
      ($exeItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "拒绝 reparse uninstaller 路径：$path"
  }
  $path
}

function Stage-UninstallerForReentry(
  [string] $uninstaller, [string] $phase, [string] $logDir) {
  Assert-NoReparsePath $logDir
  $stageDir = Join-Path $logDir `
    "uninstaller-$phase-$([Guid]::NewGuid().ToString('N'))"
  New-Item -ItemType Directory -Path $stageDir | Out-Null
  Assert-NoReparsePath $stageDir
  $staged = Join-Path $stageDir (Split-Path -Leaf $uninstaller)
  $data = [IO.Path]::ChangeExtension($uninstaller, '.dat')
  Need $data 'Inno uninstaller 缺少配套 .dat，无法保留可重入副本。'
  if (((Get-Item -LiteralPath $data).Attributes -band
       [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "拒绝 reparse uninstaller data：$data"
  }
  $exeHash = (Get-FileHash -LiteralPath $uninstaller -Algorithm SHA256).Hash
  $dataHash = (Get-FileHash -LiteralPath $data -Algorithm SHA256).Hash
  Copy-Item -LiteralPath $uninstaller -Destination $staged -Force
  $stagedData = [IO.Path]::ChangeExtension($staged, '.dat')
  Copy-Item -LiteralPath $data -Destination $stagedData -Force
  $message = [IO.Path]::ChangeExtension($uninstaller, '.msg')
  if (Test-Path -LiteralPath $message) {
    Copy-Item -LiteralPath $message -Destination `
      ([IO.Path]::ChangeExtension($staged, '.msg')) -Force
  }
  $stagedExeHash = (Get-FileHash -LiteralPath $staged -Algorithm SHA256).Hash
  $stagedDataHash = (Get-FileHash -LiteralPath $stagedData -Algorithm SHA256).Hash
  if ($stagedExeHash -cne $exeHash -or $stagedDataHash -cne $dataHash) {
    throw "retained uninstaller hash mismatch: $stageDir"
  }
  [pscustomobject]@{
    Path = $staged
    ExeSha256 = $stagedExeHash
    DataSha256 = $stagedDataHash
  }
}

function Get-UninstallFootprints(
  [string] $installRoot, [string] $uninstaller = '') {
  # The fixed root itself may intentionally retain unrelated user backups.
  # Assert every product-owned subtree and exact Inno carrier instead of
  # requiring the whole root to disappear. Test-FamoHealth covers machine and
  # exact-user registration, TIP, Run, journal, and recovery tasks.
  $uninstallerData = if ($uninstaller) {
    [IO.Path]::ChangeExtension($uninstaller, '.dat')
  } else { '' }
  [pscustomobject]@{
    BrandPresent = Test-Path -LiteralPath $BrandKey
    DeleteAnchorPresent = Test-Path -LiteralPath $UninstallDeleteAnchorKey
    UninstallEntryPresent = Test-Path -LiteralPath $UninstallKey
    VersionsPresent = Test-Path -LiteralPath (Join-Path $installRoot 'versions')
    BridgePresent = Test-Path -LiteralPath (Join-Path $installRoot 'bridge')
    PendingPresent = Test-Path -LiteralPath (Join-Path $installRoot 'pending')
    UninstallerPresent = $uninstaller -and
      (Test-Path -LiteralPath $uninstaller)
    UninstallerDataPresent = $uninstallerData -and
      (Test-Path -LiteralPath $uninstallerData)
  }
}

function Test-NotInstalledFootprints($footprints) {
  -not $footprints.BrandPresent -and
    -not $footprints.DeleteAnchorPresent -and
    -not $footprints.UninstallEntryPresent -and
    -not $footprints.VersionsPresent -and
    -not $footprints.BridgePresent -and
    -not $footprints.PendingPresent -and
    -not $footprints.UninstallerPresent -and
    -not $footprints.UninstallerDataPresent
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
                 "/LOG=`"$logPath`"")
  if ($failPhase) { $arguments += "/FamoFail=$failPhase" }
  $run = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -Wait
  $run.ExitCode
}

function Invoke-Uninstaller([string] $exe, [string] $failPhase, [string] $logPath) {
  $arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART',
                 "/LOG=`"$logPath`"")
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
Need $HealthScript 'fault matrix 需要 Test-FamoHealth.ps1 绑定 journal SID 与机器终态。'

$baseline = Get-Projection
if ($CleanInstall) {
  if ($baseline.Present) {
    throw "clean-install 模式要求机器上没有已安装的 Famo（现为 $($baseline.InstallState)）。"
  }
  $baselineTip = 'not-installed'
} else {
  if (-not $baseline.Present) { throw '没有已安装的 Famo 投影可作为回滚基线。' }
  if ($baseline.InstallState -ne 'Ready') {
    throw "基线状态不是 Ready（$($baseline.InstallState)）；先把机器恢复到 Ready 再跑。"
  }
  $baselineTip = Get-TipState $baseline.InstallDir
}
$mode = if ($Uninstall) { 'uninstall' } elseif ($CleanInstall) {
  'clean-install'
} else { 'upgrade' }
$baselineExpectedState = if ($CleanInstall) { 'NotInstalled' } else { 'Ready' }
$baselineHealth = Invoke-HealthAudit $baselineExpectedState
if (-not $baselineHealth.Passed) {
  throw "health precondition failed: expected=$baselineExpectedState actual=$($baselineHealth.ActualState)"
}

Write-Host '== Famo fault-injection harness ==' -ForegroundColor Cyan
Write-Host "Installer : $Installer"
Write-Host "Mode      : $mode"
Write-Host "Baseline  : ABI=$($baseline.BridgeAbi) state=$($baseline.InstallState) tip=$baselineTip"
Write-Host "Phases    : $($Phases -join ', ')"

if ($DryRun) {
  if ($Uninstall) { Write-Host "Uninstaller: $(Get-UninstallerPath)" }
  Write-Host 'DRY RUN PASS: inputs resolved, baseline healthy, machine untouched.' -ForegroundColor Green
  exit 0
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw '请在一个管理员 PowerShell 窗口中运行本脚本；脚本不会自行触发 UAC。'
}

$logRoot = Join-Path $InstallerDir 'dist\fault-logs'
New-Item -ItemType Directory -Force -Path $logRoot | Out-Null
Assert-NoReparsePath $logRoot
$runName = 'run-{0}-{1}' -f `
  [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'),
  [Guid]::NewGuid().ToString('N')
$logDir = Join-Path $logRoot $runName
New-Item -ItemType Directory -Path $logDir | Out-Null
Assert-NoReparsePath $logDir
Initialize-StepResultFile
$steps = @()
$aborted = $null

if ($Uninstall) {
  $installRoot = ''
  $uninstaller = ''
  foreach ($phase in $Phases) {
    Write-Host "-- uninstall inject $phase" -ForegroundColor Yellow
    $beforeHealth = Invoke-HealthAudit 'Ready'
    $phaseBaseline = Get-Projection
    if (-not $beforeHealth.Passed -or -not $phaseBaseline.Present -or
        $phaseBaseline.InstallState -ne 'Ready') {
      $aborted = "uninstall 注入 $phase 的起点不是 Ready"
      $step = [pscustomobject]@{
        Step = "uninstall-inject:$phase"
        Expect = 'fresh Ready precondition'
        BeforeHealth = $beforeHealth
        Passed = $false
      }
      $steps += $step
      Write-StepResult $step
      break
    }

    $uninstaller = Get-UninstallerPath
    $installRoot = Split-Path -Parent $uninstaller
    $stagedUninstaller = Stage-UninstallerForReentry `
      $uninstaller $phase $logDir
    $log = Join-Path $logDir "fail-$phase.log"
    $exit = Invoke-Uninstaller $uninstaller $phase $log
    $injectionObserved = Test-InjectionLog $log $phase
    $afterInjection = Get-UninstallFootprints $installRoot $uninstaller

    $recoveryLog = Join-Path $logDir "recover-$phase.log"
    $recoveryExit = -1
    $reentryUninstaller = ''
    if ($exit -ne 0) {
      $reentryUninstaller = $stagedUninstaller.Path
      $recoveryExit = Invoke-Uninstaller $reentryUninstaller '' $recoveryLog
    }
    $afterRecovery = Get-UninstallFootprints $installRoot $uninstaller
    $afterHealth = Invoke-HealthAudit 'NotInstalled'
    $notInstalled = (Test-NotInstalledFootprints $afterRecovery) -and
      $afterHealth.Passed

    $reinstallExit = $null
    $readyForNextPhase = $phase -eq $Phases[-1]
    $reinstallState = if ($readyForNextPhase) { 'not-required' } else { '' }
    if (($exit -ne 0) -and $injectionObserved -and
        ($recoveryExit -eq 0) -and $notInstalled -and
        -not $readyForNextPhase) {
      $reinstallLog = Join-Path $logDir "reinstall-after-$phase.log"
      $reinstallExit = Invoke-Installer $Installer '' $reinstallLog
      $reinstalled = Get-Projection
      $reinstallTip = if ($reinstalled.Present) {
        Get-TipState $reinstalled.InstallDir
      } else { 'no-install' }
      $reinstallHealth = Invoke-HealthAudit 'Ready'
      $readyForNextPhase = ($reinstallExit -eq 0) -and
        $reinstalled.Present -and ($reinstalled.InstallState -eq 'Ready') -and
        ($reinstallTip -eq 'present') -and $reinstallHealth.Passed
      $reinstallState = if ($reinstalled.Present) {
        $reinstalled.InstallState
      } else { 'absent' }
    }

    $ok = ($exit -ne 0) -and $injectionObserved -and
      ($recoveryExit -eq 0) -and $notInstalled -and $readyForNextPhase
    $step = [pscustomobject]@{
      Step = "uninstall-inject:$phase"
      Expect = 'injected failure + clean re-entry to NotInstalled'
      BeforeHealth = $beforeHealth
      InjectionExitCode = $exit
      InjectionObserved = $injectionObserved
      RecoveryExitCode = $recoveryExit
      RecoveryUninstaller = $reentryUninstaller
      RetainedUninstallerExeSha256 = $stagedUninstaller.ExeSha256
      RetainedUninstallerDataSha256 = $stagedUninstaller.DataSha256
      NotInstalled = $notInstalled
      AfterInjection = $afterInjection
      AfterRecovery = $afterRecovery
      AfterHealth = $afterHealth
      ReinstallExitCode = $reinstallExit
      ReinstallState = $reinstallState
      ReadyForNextPhase = $readyForNextPhase
      Passed = $ok
      Log = $log
      RecoveryLog = $recoveryLog
    }
    $steps += $step
    Write-StepResult $step
    if (-not $ok) {
      $aborted = "uninstall 注入点未能干净恢复：$phase"
      Write-Host "   FAIL inject=$exit recover=$recoveryExit notInstalled=$notInstalled readyForNext=$readyForNextPhase" -ForegroundColor Red
      break
    }
    Write-Host "   ok (inject=$exit, recover=$recoveryExit, NotInstalled; next=$readyForNextPhase)" -ForegroundColor Green
  }

  $final = Get-Projection
  $finalTip = 'absent'
  $finalFootprints = Get-UninstallFootprints $installRoot $uninstaller
  $finalHealth = Invoke-HealthAudit 'NotInstalled'
  $healthy = (Test-NotInstalledFootprints $finalFootprints) -and
    $finalHealth.Passed
  $result = [pscustomobject]@{
    mode = $mode
    installer = $Installer
    installerSha256 = (Get-FileHash -LiteralPath $Installer -Algorithm SHA256).Hash
    baseline = $baseline
    baselineTip = $baselineTip
    baselineHealth = $baselineHealth
    steps = $steps
    final = $final
    finalTip = $finalTip
    finalFootprints = $finalFootprints
    finalHealth = $finalHealth
    stepResultPath = $StepResultPath
    machineHealthy = $healthy
    aborted = $aborted
    passed = (-not $aborted) -and $healthy -and
      (($steps | Where-Object { -not $_.Passed }).Count -eq 0)
  }
  New-Item -ItemType Directory -Force -Path `
    (Split-Path $ResultPath -Parent) | Out-Null
  $result | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath $ResultPath -Encoding UTF8
  Write-Host "result -> $ResultPath"
  if (-not $healthy) {
    Write-Host 'MACHINE NOT CLEAN: rerun the retained same-version uninstaller before any setup.' -ForegroundColor Red
  }
  if ($result.passed) {
    Write-Host 'UNINSTALL FAULT INJECTION PASS' -ForegroundColor Green
    exit 0
  }
  Write-Host "UNINSTALL FAULT INJECTION FAIL: $aborted" -ForegroundColor Red
  exit 1
}

foreach ($phase in $Phases) {
  Write-Host "-- inject $phase" -ForegroundColor Yellow
  if ($CleanInstall) {
    $beforeHealth = Invoke-HealthAudit 'NotInstalled'
    if (-not $beforeHealth.Passed) {
      $aborted = "clean install 注入 $phase 的起点不是 NotInstalled"
      $step = [pscustomobject]@{
        Step = "clean-inject:$phase"
        Expect = 'fresh NotInstalled precondition'
        BeforeHealth = $beforeHealth
        Passed = $false
      }
      $steps += $step
      Write-StepResult $step
      break
    }
  } else {
    $beforeHealth = Invoke-HealthAudit 'Ready'
    if (-not $beforeHealth.Passed) {
      $aborted = "升级注入 $phase 的起点不是 Ready"
      $step = [pscustomobject]@{
        Step = "inject:$phase"
        Expect = 'fresh Ready precondition'
        BeforeHealth = $beforeHealth
        Passed = $false
      }
      $steps += $step
      Write-StepResult $step
      break
    }
  }
  $log = Join-Path $logDir "fail-$phase.log"
  $exit = Invoke-Installer $Installer $phase $log
  $injectionObserved = Test-InjectionLog $log $phase
  $after = Get-Projection

  if ($CleanInstall) {
    # A failed first install may retain a RolledBack tombstone, but it must not
    # leave a usable projection; the canonical audit also requires TIP absent.
    $afterHealth = Invoke-HealthAudit 'NotInstalled'
    $usable = $after.Present -and ($after.InstallState -eq 'Ready')
    $ok = ($exit -ne 0) -and $injectionObserved -and (-not $usable) -and
      $afterHealth.Passed
    $step = [pscustomobject]@{
      Step = "clean-inject:$phase"
      Expect = 'fail + nothing left Ready + TIP absent'
      BeforeHealth = $beforeHealth
      ExitCode = $exit
      InjectionObserved = $injectionObserved
      InstallState = if ($after.Present) { $after.InstallState } else { 'absent' }
      Tip = if ($afterHealth.Passed) { 'absent' } else { 'audit-failed' }
      AfterHealth = $afterHealth
      Passed = $ok
      Log = $log
    }
    $steps += $step
    Write-StepResult $step
    if (-not $ok) {
      $aborted = "clean install 注入后残留可用投影或 TIP：$phase"
      Write-Host "   FAIL exit=$exit marker=$injectionObserved state=$($after.InstallState) health=$($afterHealth.ActualState)" -ForegroundColor Red
      break
    }
    Write-Host "   ok (exit=$exit, no Ready projection, TIP absent)" -ForegroundColor Green
    continue
  }

  $tip = if ($after.Present) { Get-TipState $after.InstallDir } else { 'no-install' }
  $drift = Compare-Projection $baseline $after
  $afterHealth = Invoke-HealthAudit 'RolledBack'

  # A rejected transaction must fail loudly, land on the RolledBack terminal
  # state, and leave the artifact projection untouched. A zero exit would mean
  # the injection point never fired.
  $injectionOk = ($exit -ne 0) -and $injectionObserved -and
    ($drift.Count -eq 0) -and ($tip -eq $baselineTip) -and
    ($after.InstallState -eq 'RolledBack') -and $afterHealth.Passed
  $recoveryExit = $null
  $recoveryHealth = $null
  $readyForNextPhase = ($phase -eq $Phases[-1]) -and (-not $SkipMigration)

  # Every later phase must start from Ready, or it is testing a different
  # transition than the one it names. Recovery is a full installer run, and it
  # lands on PendingReboot whenever the Bridge is loaded -- which cannot be
  # cleared without a logon. So a multi-phase sweep is only unattended on a
  # machine where the Bridge is not loaded; otherwise stop and say so.
  if ($injectionOk -and -not $readyForNextPhase) {
    $recoverLog = Join-Path $logDir "recover-after-$phase.log"
    $recoveryExit = Invoke-Installer $Installer '' $recoverLog
    $recovered = Get-Projection
    $recoveryHealth = Invoke-HealthAudit 'Ready'
    $readyForNextPhase = ($recoveryExit -eq 0) -and
      ($recovered.InstallState -eq 'Ready') -and $recoveryHealth.Passed
    if ($readyForNextPhase) { $baseline = $recovered }
  }

  $ok = $injectionOk -and $readyForNextPhase
  $step = [pscustomobject]@{
    Step = "inject:$phase"; Expect = 'fail + RolledBack + exact artifact rollback'
    BeforeHealth = $beforeHealth
    ExitCode = $exit; InjectionObserved = $injectionObserved
    InstallState = $after.InstallState; Drift = $drift
    Tip = $tip; AfterHealth = $afterHealth
    RecoveryExitCode = $recoveryExit; RecoveryHealth = $recoveryHealth
    ReadyForNextPhase = $readyForNextPhase
    Passed = $ok; Log = $log
  }
  $steps += $step
  Write-StepResult $step
  if (-not $ok) {
    $aborted = if (-not $injectionOk) {
      "回滚未还原到基线：$phase"
    } else {
      "注入 $phase 后无法在不重启的情况下回到 Ready"
    }
    Write-Host "   FAIL exit=$exit marker=$injectionObserved state=$($after.InstallState) drift=$($drift -join '; ') tip=$tip readyForNext=$readyForNextPhase" -ForegroundColor Red
    break
  }
  Write-Host "   ok (exit=$exit, state=RolledBack, artifacts unchanged, tip=$tip)" -ForegroundColor Green
}

if (-not $aborted -and -not $SkipMigration -and -not $CleanInstall) {
  Write-Host '-- clean migration (no injection)' -ForegroundColor Yellow
  $log = Join-Path $logDir 'migrate.log'
  $exit = Invoke-Installer $Installer '' $log
  $after = Get-Projection
  $tip = Get-TipState $after.InstallDir
  $migrationHealth = Invoke-HealthAudit 'Ready'
  $v3Intact = ($baseline.BridgeFileHash -eq
    (Get-FileHash -LiteralPath $baseline.BridgePath -Algorithm SHA256 -ErrorAction SilentlyContinue).Hash)
  $ok = ($exit -eq 0) -and ($after.InstallState -eq 'Ready') -and
        ($after.BridgeAbi -ne $baseline.BridgeAbi) -and ($tip -eq 'present') -and
        $v3Intact -and $migrationHealth.Passed
  $step = [pscustomobject]@{
    Step = 'migrate'; Expect = 'Ready on new ABI, previous bridge bytes intact'
    ExitCode = $exit; FromAbi = $baseline.BridgeAbi; ToAbi = $after.BridgeAbi
    InstallState = $after.InstallState; Tip = $tip; PreviousBridgeIntact = $v3Intact
    Health = $migrationHealth; Passed = $ok; Log = $log
  }
  $steps += $step
  Write-StepResult $step
  if (-not $ok) { $aborted = '迁移未达成 Ready/新 ABI/旧工件完好' }
  else {
    Write-Host "   ok (ABI $($baseline.BridgeAbi) -> $($after.BridgeAbi), tip=$tip)" -ForegroundColor Green

    Write-Host '-- repair idempotence (same installer again)' -ForegroundColor Yellow
    $before = Get-Projection
    $log = Join-Path $logDir 'repair.log'
    $exit = Invoke-Installer $Installer '' $log
    $now = Get-Projection
    $repairHealth = Invoke-HealthAudit 'Ready'
    # A repair may mint a new runtime target; the Bridge must not move.
    $bridgeDrift = @('BridgePath', 'BridgeAbi', 'BridgeHash', 'BridgeFileHash',
                     'BridgeFileId') |
      Where-Object { $before.$_ -cne $now.$_ }
    $ok = ($exit -eq 0) -and ($now.InstallState -eq 'Ready') -and
          ($bridgeDrift.Count -eq 0) -and $repairHealth.Passed
    $step = [pscustomobject]@{
      Step = 'repair'; Expect = 'Ready, bridge untouched'
      ExitCode = $exit; BridgeDrift = $bridgeDrift
      InstallState = $now.InstallState; Health = $repairHealth
      Passed = $ok; Log = $log
    }
    $steps += $step
    Write-StepResult $step
    if (-not $ok) { $aborted = 'repair 非幂等或触碰了 Bridge' }
    else { Write-Host '   ok (bridge untouched)' -ForegroundColor Green }
  }
}

$final = Get-Projection
$finalExpectedState = if ($CleanInstall) { 'NotInstalled' } else { 'Ready' }
$finalHealth = Invoke-HealthAudit $finalExpectedState
if ($CleanInstall) {
  $finalTip = if ($finalHealth.Passed) { 'absent' } else { 'audit-failed' }
  $healthy = -not ($final.Present -and $final.InstallState -eq 'Ready') -and
    $finalHealth.Passed
} else {
  $finalTip = if ($final.Present) {
    Get-TipState $final.InstallDir
  } else { 'no-install' }
  $healthy = $final.Present -and ($final.InstallState -eq 'Ready') -and
    ($finalTip -eq 'present') -and $finalHealth.Passed
}

$result = [pscustomobject]@{
  mode        = $mode
  installer   = $Installer
  installerSha256 = (Get-FileHash -LiteralPath $Installer -Algorithm SHA256).Hash
  baseline    = $baseline
  baselineTip = $baselineTip
  baselineHealth = $baselineHealth
  steps       = $steps
  final       = $final
  finalTip    = $finalTip
  finalHealth = $finalHealth
  stepResultPath = $StepResultPath
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
  Write-Host '恢复：用写下该事务的同版或更新版安装器重跑修复；不要降级恢复。' -ForegroundColor Red
}
if ($result.passed) {
  Write-Host 'FAULT INJECTION PASS' -ForegroundColor Green
  exit 0
}
Write-Host "FAULT INJECTION FAIL: $aborted" -ForegroundColor Red
exit 1
