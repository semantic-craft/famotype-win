[CmdletBinding()]
param(
  [ValidateRange(5, 3600)]
  [int] $DurationSeconds = 60,
  [ValidateRange(10, 1000)]
  [int] $PressureIntervalMs = 40,
  [string] $EvidenceDirectory = (Join-Path $env:LOCALAPPDATA 'Famo\diagnostics\explorer-hang-probes'),
  [switch] $CaptureDumpOnHang,
  [switch] $AllowExplorerRestart,
  [switch] $AllowTemporaryRegistration,
  [switch] $PreflightOnly,
  [switch] $SelfCheck
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$FamoClsid = '{54EAD76A-B864-4A6D-9C82-148E3352BEE7}'
$FamoTip = '0804:{54EAD76A-B864-4A6D-9C82-148E3352BEE7}{0158C2BA-4E96-4BA8-B505-E1BBEBB3FA33}'
$MicrosoftPinyinTip = '0804:{81D4E9C9-1D3B-41BC-9E6C-4B40BF79E35E}{FA550B04-5AD7-411F-A5AC-CA038EC515D7}'
$ProbeScript = Join-Path $PSScriptRoot 'Invoke-FamoExplorerHangProbe.ps1'
$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..'))
$RegistrationPath = "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\CLSID\$FamoClsid\InProcServer32"
$BrandPath = 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Famo\InputMethod'
$StagingPayload = [System.IO.Path]::GetFullPath(
  (Join-Path $PSScriptRoot '..\..\installer\staging\payload'))
$StagingBridge = [System.IO.Path]::GetFullPath(
  (Join-Path $PSScriptRoot '..\..\installer\staging\bridge'))
$TipRegistrationPath = "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\CTF\TIP\$FamoClsid"
$TipRegistrationKey = "HKLM\SOFTWARE\Microsoft\CTF\TIP\$FamoClsid"

function Resolve-Comparison {
  param([string] $MicrosoftVerdict, [string] $FamoVerdict)
  if ($MicrosoftVerdict -eq 'green' -and $FamoVerdict -eq 'red') { return 'famo-only-red-needs-dump-analysis' }
  if ($MicrosoftVerdict -eq 'red') { return 'microsoft-also-red' }
  if ($MicrosoftVerdict -eq 'green' -and $FamoVerdict -eq 'green') { return 'not-reproduced' }
  return 'incomplete-or-inconclusive'
}

function Resolve-ExitCode {
  param([string] $Comparison, [bool] $CleanupPassed)
  if (-not $CleanupPassed) { return 2 }
  if ($Comparison -eq 'famo-only-red-needs-dump-analysis') { return 1 }
  if ($Comparison -eq 'not-reproduced') { return 0 }
  return 2
}

function Resolve-FamoArtifactPaths {
  param(
    [Parameter(Mandatory = $true)][bool] $RegistrationPresent,
    [string] $RegisteredDll,
    [string] $InstallDir,
    [string] $BridgePath,
    [Parameter(Mandatory = $true)][string] $FallbackPayload,
    [Parameter(Mandatory = $true)][string] $FallbackBridge
  )

  if ($RegistrationPresent) {
    if ([string]::IsNullOrWhiteSpace($RegisteredDll)) {
      throw 'Famo COM registration exists without an InProcServer32 default value.'
    }
    if (-not $InstallDir) {
      throw 'Famo COM registration exists without a machine InstallDir.'
    }
    $dll = [System.IO.Path]::GetFullPath($RegisteredDll.Trim('"'))
    $target = [System.IO.Path]::GetFullPath($InstallDir.Trim('"')).TrimEnd('\')
    if ((Split-Path -Leaf $dll) -ine 'FamoTextService.dll') {
      throw "Famo COM registration points to an unsupported text service: $dll"
    }
    $expectedBridge = if ($BridgePath) {
      [System.IO.Path]::GetFullPath($BridgePath.Trim('"'))
    } else {
      Join-Path $target 'FamoTextService.dll'
    }
    if (-not [string]::Equals(
        $dll,
        $expectedBridge,
        [System.StringComparison]::OrdinalIgnoreCase)) {
      throw "Famo COM registration does not match Bridge projection: $dll <> $expectedBridge"
    }
    $source = 'RegisteredInstall'
  } else {
    $target = [System.IO.Path]::GetFullPath($FallbackPayload).TrimEnd('\')
    $dll = Join-Path ([System.IO.Path]::GetFullPath($FallbackBridge)) 'FamoTextService.dll'
    $source = 'StagingPayload'
  }

  return [pscustomobject]@{
    source = $source
    dll = $dll
    activationAssembly = Join-Path $target 'settings\Famo.Settings.Core.dll'
  }
}

if ($SelfCheck) {
  $cases = @(
    @{ expected='famo-only-red-needs-dump-analysis'; actual=(Resolve-Comparison green red) },
    @{ expected='microsoft-also-red'; actual=(Resolve-Comparison red green) },
    @{ expected='not-reproduced'; actual=(Resolve-Comparison green green) },
    @{ expected='incomplete-or-inconclusive'; actual=(Resolve-Comparison green inconclusive) },
    @{ expected=0; actual=(Resolve-ExitCode 'not-reproduced' $true) },
    @{ expected=1; actual=(Resolve-ExitCode 'famo-only-red-needs-dump-analysis' $true) },
    @{ expected=2; actual=(Resolve-ExitCode 'not-reproduced' $false) }
  )
  $fixtureArgs = @{
    RegistrationPresent = $true
    RegisteredDll = 'C:\Program Files\Famo\bridge\v1\FamoTextService.dll'
    InstallDir = 'C:\Program Files\Famo\versions\1.5.3-fixture'
    BridgePath = 'C:\Program Files\Famo\bridge\v1\FamoTextService.dll'
    FallbackPayload = 'C:\staging\payload'
    FallbackBridge = 'C:\staging\bridge'
  }
  $fixture = Resolve-FamoArtifactPaths @fixtureArgs
  $legacyRejected = $false
  try {
    $legacyArgs = @{
      RegistrationPresent = $true
      RegisteredDll = 'C:\Program Files\Famo\FamoTsf.dll'
      InstallDir = 'C:\Program Files\Famo'
      BridgePath = 'C:\Program Files\Famo\bridge\v1\FamoTextService.dll'
      FallbackPayload = 'C:\staging\payload'
      FallbackBridge = 'C:\staging\bridge'
    }
    [void](Resolve-FamoArtifactPaths @legacyArgs)
  } catch {
    $legacyRejected = $true
  }
  $emptyRegistrationRejected = $false
  try {
    $emptyRegistrationArgs = @{
      RegistrationPresent = $true
      RegisteredDll = ''
      InstallDir = 'C:\Program Files\Famo\versions\1.5.3-fixture'
      BridgePath = 'C:\Program Files\Famo\bridge\v1\FamoTextService.dll'
      FallbackPayload = 'C:\staging\payload'
      FallbackBridge = 'C:\staging\bridge'
    }
    [void](Resolve-FamoArtifactPaths @emptyRegistrationArgs)
  } catch {
    $emptyRegistrationRejected = $true
  }
  $cases += @(
    @{ expected='RegisteredInstall'; actual=$fixture.source },
    @{ expected='FamoTextService.dll'; actual=(Split-Path -Leaf $fixture.dll) },
    @{ expected='Famo.Settings.Core.dll'; actual=(Split-Path -Leaf $fixture.activationAssembly) },
    @{ expected=$true; actual=$legacyRejected },
    @{ expected=$true; actual=$emptyRegistrationRejected }
  )
  $failures = @($cases | Where-Object { $_.actual -ne $_.expected })
  [pscustomobject]@{ mode='SelfCheck'; passed=($failures.Count -eq 0); cases=$cases } | ConvertTo-Json -Depth 4
  if ($failures.Count -gt 0) { exit 1 }
  exit 0
}

$evidenceFull = [System.IO.Path]::GetFullPath($EvidenceDirectory)
$repoPrefix = $RepoRoot.TrimEnd('\') + '\'
if ($evidenceFull.TrimEnd('\') -ieq $RepoRoot.TrimEnd('\') -or $evidenceFull.StartsWith($repoPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
  throw 'EvidenceDirectory must be outside the repository.'
}
$EvidenceDirectory = $evidenceFull

$registrationWasPresent = Test-Path -LiteralPath $RegistrationPath
$registeredDll = if ($registrationWasPresent) {
  [string](Get-Item -LiteralPath $RegistrationPath).GetValue('')
} else { '' }
$installDir = if (Test-Path -LiteralPath $BrandPath) {
  [string](Get-ItemPropertyValue -LiteralPath $BrandPath -Name InstallDir -ErrorAction SilentlyContinue)
} else { '' }
$bridgePath = if (Test-Path -LiteralPath $BrandPath) {
  [string](Get-ItemPropertyValue -LiteralPath $BrandPath -Name BridgePath -ErrorAction SilentlyContinue)
} else { '' }
$artifactArgs = @{
  RegistrationPresent = $registrationWasPresent
  RegisteredDll = $registeredDll
  InstallDir = $installDir
  BridgePath = $bridgePath
  FallbackPayload = $StagingPayload
  FallbackBridge = $StagingBridge
}
$artifacts = Resolve-FamoArtifactPaths @artifactArgs
$FamoDll = $artifacts.dll
$ActivationAssembly = $artifacts.activationAssembly

if (-not (Test-Path -LiteralPath $ProbeScript)) { throw "Probe script missing: $ProbeScript" }
if (-not (Test-Path -LiteralPath $FamoDll)) { throw "Installed Famo DLL missing: $FamoDll" }
if (-not (Test-Path -LiteralPath $ActivationAssembly)) { throw "Famo activation assembly missing: $ActivationAssembly" }
if ($PreflightOnly) {
  [pscustomobject]@{
    mode = 'Preflight'
    passed = $true
    source = $artifacts.source
    registered = $registrationWasPresent
    dll = $FamoDll
    activationAssembly = $ActivationAssembly
  } | ConvertTo-Json -Depth 3
  exit 0
}
if ($PSVersionTable.PSEdition -ne 'Core') {
  throw 'Run this A/B orchestrator with pwsh; Windows PowerShell 5.1 cannot load the .NET 8 activation assembly.'
}
if (-not $AllowExplorerRestart) { throw 'Pass -AllowExplorerRestart after saving open Explorer work.' }

function Restart-ShellExplorer {
  Write-Warning 'Explorer will restart now. Open Explorer windows may close.'
  Get-Process -Name explorer -ErrorAction SilentlyContinue | Stop-Process -Force
  $deadline = (Get-Date).AddSeconds(15)
  do {
    Start-Sleep -Milliseconds 250
    $process = Get-Process -Name explorer -ErrorAction SilentlyContinue | Sort-Object StartTime -Descending | Select-Object -First 1
    if (-not $process -and (Get-Date) -gt $deadline.AddSeconds(-12)) { Start-Process explorer.exe }
  } until ($process -or (Get-Date) -ge $deadline)
  if (-not $process) { throw 'Explorer did not restart within 15 seconds.' }
  Start-Sleep -Milliseconds 1200
  return $process.Id
}

function Set-TemporaryInputTips {
  $languages = Get-WinUserLanguageList
  $chinese = $null
  foreach ($language in $languages) {
    if ($language.LanguageTag -eq 'zh-Hans-CN') { $chinese = $language; break }
  }
  if (-not $chinese) { throw 'zh-Hans-CN is not present in the current user language list.' }
  foreach ($tip in @($MicrosoftPinyinTip, $FamoTip)) {
    if (-not @($chinese.InputMethodTips | Where-Object { $_ -ieq $tip }).Count) {
      $chinese.InputMethodTips.Add($tip)
    }
  }
  Set-WinUserLanguageList -LanguageList $languages -Force
}

function Invoke-ProfileProbe {
  param([string] $Label, [string] $Tip)
  Set-WinDefaultInputMethodOverride -InputTip $Tip
  $explorerPid = Restart-ShellExplorer
  $activated = $null
  if ($Label -eq 'famo') {
    Add-Type -Path $ActivationAssembly
    $activated = [Famo.Settings.Core.InputMethodList]::ActivateFamoForCurrentDesktop()
    if (-not $activated) { throw 'ActivateFamoForCurrentDesktop returned false.' }
  }
  $arguments = @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $ProbeScript,
    '-Label', $Label,
    '-DurationSeconds', $DurationSeconds,
    '-PressureIntervalMs', $PressureIntervalMs,
    '-EvidenceDirectory', $EvidenceDirectory,
    '-FamoModulePath', $FamoDll,
    '-GeneratePressure'
  )
  if ($CaptureDumpOnHang) { $arguments += '-CaptureDumpOnHang' }
  $raw = & powershell @arguments
  $exitCode = $LASTEXITCODE
  try { $result = $raw | ConvertFrom-Json } catch { throw "Probe output was not JSON for $Label (exit $exitCode)." }
  return [pscustomobject]@{ explorerPid = $explorerPid; profileActivated = $activated; probeExitCode = $exitCode; result = $result }
}

function Invoke-ElevatedRegsvr32 {
  param([switch] $Unregister)
  $arguments = if ($Unregister) { '/u /s "{0}"' -f $FamoDll } else { '/s "{0}"' -f $FamoDll }
  $process = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\regsvr32.exe') -ArgumentList $arguments -Verb RunAs -Wait -PassThru
  if ($process.ExitCode -ne 0) { throw "regsvr32 exited $($process.ExitCode)" }
}

function Remove-ElevatedTipRegistration {
  if (-not (Test-Path -LiteralPath $TipRegistrationPath)) { return }
  $process = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\reg.exe') -ArgumentList @('DELETE', $TipRegistrationKey, '/f') -Verb RunAs -Wait -PassThru
  if ($process.ExitCode -ne 0 -and (Test-Path -LiteralPath $TipRegistrationPath)) {
    throw "reg.exe DELETE exited $($process.ExitCode)"
  }
}

$originalLanguages = Get-WinUserLanguageList
$originalOverride = [string](Get-ItemPropertyValue -Path 'HKCU:\Control Panel\International\User Profile' -Name InputMethodOverride -ErrorAction SilentlyContinue)
$temporaryRegistration = $false
$inputStateChanged = $false
$runs = New-Object System.Collections.Generic.List[object]
$cleanup = New-Object System.Collections.Generic.List[object]
$startedUtc = (Get-Date).ToUniversalTime().ToString('o')

try {
  if (-not $registrationWasPresent) {
    if (-not $AllowTemporaryRegistration) { throw 'Famo TSF is not registered. Pass -AllowTemporaryRegistration to reproduce the current DLL under UAC.' }
    Invoke-ElevatedRegsvr32
    if (-not (Test-Path -LiteralPath $RegistrationPath)) { throw 'Famo registration is still absent after regsvr32.' }
    $temporaryRegistration = $true
  }

  Set-TemporaryInputTips
  $inputStateChanged = $true
  $runs.Add((Invoke-ProfileProbe 'microsoft-pinyin' $MicrosoftPinyinTip))
  $runs.Add((Invoke-ProfileProbe 'famo' $FamoTip))
} finally {
  if ($inputStateChanged) {
    try {
      Set-WinUserLanguageList -LanguageList $originalLanguages -Force
      if ($originalOverride) {
        Set-WinDefaultInputMethodOverride -InputTip $originalOverride
      } else {
        Remove-ItemProperty -Path 'HKCU:\Control Panel\International\User Profile' -Name InputMethodOverride -ErrorAction SilentlyContinue
      }
      $cleanup.Add([pscustomobject]@{ action = 'restore-input-state'; pass = $true })
      Restart-ShellExplorer | Out-Null
      $cleanup.Add([pscustomobject]@{ action = 'restart-restored-explorer'; pass = $true })
    } catch {
      $cleanup.Add([pscustomobject]@{ action = 'restore-input-state'; pass = $false; error = $_.Exception.Message })
    }
  }

  if ($temporaryRegistration) {
    try {
      Invoke-ElevatedRegsvr32 -Unregister
      Remove-ElevatedTipRegistration
      $registrationRemoved = -not (Test-Path -LiteralPath $RegistrationPath)
      $tipRegistrationRemoved = -not (Test-Path -LiteralPath $TipRegistrationPath)
      $cleanup.Add([pscustomobject]@{ action = 'remove-temporary-registration'; pass = ($registrationRemoved -and $tipRegistrationRemoved); clsidRemoved = $registrationRemoved; tipRegistrationRemoved = $tipRegistrationRemoved })
    } catch {
      $cleanup.Add([pscustomobject]@{ action = 'remove-temporary-registration'; pass = $false; error = $_.Exception.Message })
    }
  }
}

$microsoft = @($runs | Where-Object { $_.result.label -eq 'microsoft-pinyin' } | Select-Object -First 1)
$famo = @($runs | Where-Object { $_.result.label -eq 'famo' } | Select-Object -First 1)
$comparison = if ($microsoft.Count -eq 1 -and $famo.Count -eq 1) {
  Resolve-Comparison $microsoft[0].result.verdict $famo[0].result.verdict
} else { 'incomplete-or-inconclusive' }

New-Item -ItemType Directory -Force -Path $EvidenceDirectory | Out-Null
$outputPath = Join-Path $EvidenceDirectory ("ab-{0}.json" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
$result = [pscustomobject]@{
  schemaVersion = 1
  startedUtc = $startedUtc
  endedUtc = (Get-Date).ToUniversalTime().ToString('o')
  dll = [pscustomobject]@{ path = $FamoDll; sha256 = (Get-FileHash -LiteralPath $FamoDll -Algorithm SHA256).Hash; temporaryRegistration = $temporaryRegistration }
  runs = $runs.ToArray()
  comparison = $comparison
  cleanup = $cleanup.ToArray()
  evidencePath = $outputPath
}
$result | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath $outputPath -Encoding UTF8
$result | ConvertTo-Json -Depth 15

$cleanupPassed = @($cleanup | Where-Object { -not $_.pass }).Count -eq 0
exit (Resolve-ExitCode $comparison $cleanupPassed)
