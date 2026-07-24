# ============================================================================
# Get-FamoDiagnostics.ps1
# ----------------------------------------------------------------------------
# Safe local diagnostics bundle for Famo Windows support. This script is read
# only: it does not read clipboard contents, typed text, dictionaries, quick
# phrases, prompt libraries, AI provider profiles, or user Weasel data files.
#
# Usage:
#   powershell -File Get-FamoDiagnostics.ps1 [-Json] [-RecentLogLines 80]
# ============================================================================

[CmdletBinding()]
param(
  [switch] $Json,
  [int] $RecentLogLines = 80
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$identity = Get-Content (Join-Path $root 'famo-identity.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$clsid = '{' + ([string]$identity.guids.clsidTextService).ToUpperInvariant() + '}'
$profileGuid = '{' + ([string]$identity.guids.guidProfile).ToUpperInvariant() + '}'
$pipeName = [string]$identity.ipc.pipeName
$brandSubkey = [string]$identity.registry.brandKey
$famoDir = Join-Path $env:LOCALAPPDATA 'Famo'
$famoLogDir = Join-Path $famoDir 'log'
$settingsLog = Join-Path $famoLogDir 'famo-settings.log'
$timingLog = Join-Path $famoLogDir 'famo-timing.log'
$healthScript = Join-Path $PSScriptRoot 'Test-FamoHealth.ps1'
$tsfScript = Join-Path $PSScriptRoot 'Test-FamoTsfRegistration.ps1'

function Measure-Step {
  param([scriptblock] $Block)
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  try {
    $value = & $Block
    $sw.Stop()
    return @{ ok = $true; elapsedMs = [math]::Round($sw.Elapsed.TotalMilliseconds, 3); value = $value; error = $null }
  } catch {
    $sw.Stop()
    return @{ ok = $false; elapsedMs = [math]::Round($sw.Elapsed.TotalMilliseconds, 3); value = $null; error = $_.Exception.Message }
  }
}

function Open-RegistryBase {
  param(
    [Microsoft.Win32.RegistryHive] $Hive,
    [Microsoft.Win32.RegistryView] $View = [Microsoft.Win32.RegistryView]::Default
  )
  return [Microsoft.Win32.RegistryKey]::OpenBaseKey($Hive, $View)
}

function Get-RegValue {
  param(
    [Microsoft.Win32.RegistryKey] $Base,
    [string] $Path,
    [string] $Name
  )
  $key = $null
  try {
    $key = $Base.OpenSubKey($Path)
    if (-not $key) { return $null }
    return $key.GetValue($Name)
  } finally {
    if ($key) { $key.Close() }
  }
}

function Redact-Line {
  param([string] $Line)
  $redacted = $Line -replace '(?i)(api[_-]?key|token|password|secret)=\S+', '$1=<redacted>'
  $redacted = $redacted -replace 'sk-[A-Za-z0-9_-]{12,}', 'sk-<redacted>'
  return $redacted
}

function Read-LogTail {
  param([string] $Path, [int] $Lines)
  if (-not (Test-Path -LiteralPath $Path)) {
    return @()
  }

  return @(Get-Content -LiteralPath $Path -Tail $Lines -ErrorAction SilentlyContinue | ForEach-Object { Redact-Line $_ })
}

function Test-PipeTiming {
  param([string] $Name)
  $client = $null
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  try {
    $client = New-Object System.IO.Pipes.NamedPipeClientStream(
      '.', "$($env:USERNAME)\$Name", [System.IO.Pipes.PipeDirection]::InOut)
    $client.Connect(200)
    $sw.Stop()
    return [pscustomobject]@{ state = 'Ready'; elapsedMs = [math]::Round($sw.Elapsed.TotalMilliseconds, 3); detail = 'bounded pipe connect OK' }
  } catch [System.TimeoutException] {
    $sw.Stop()
    return [pscustomobject]@{ state = 'Hung'; elapsedMs = [math]::Round($sw.Elapsed.TotalMilliseconds, 3); detail = 'bounded pipe connect timed out after 200ms' }
  } catch {
    $sw.Stop()
    return [pscustomobject]@{ state = 'Broken'; elapsedMs = [math]::Round($sw.Elapsed.TotalMilliseconds, 3); detail = $_.Exception.Message }
  } finally {
    if ($client) { $client.Dispose() }
  }
}

function Get-FileVersionInfo {
  param([string] $Path)
  if (-not ($Path -and (Test-Path -LiteralPath $Path))) {
    return $null
  }

  $item = Get-Item -LiteralPath $Path
  return [pscustomobject]@{
    path = $Path
    exists = $true
    length = $item.Length
    lastWriteUtc = $item.LastWriteTimeUtc.ToString('o')
    fileVersion = $item.VersionInfo.FileVersion
    productVersion = $item.VersionInfo.ProductVersion
  }
}

$hklm64 = Open-RegistryBase ([Microsoft.Win32.RegistryHive]::LocalMachine) ([Microsoft.Win32.RegistryView]::Registry64)
$hkcu = Open-RegistryBase ([Microsoft.Win32.RegistryHive]::CurrentUser)

$installDir = [string](Get-RegValue $hklm64 $brandSubkey 'InstallDir')
$serverExe = [string](Get-RegValue $hklm64 $brandSubkey 'ServerExecutable')
$deployerPath = [string](Get-RegValue $hklm64 $brandSubkey 'FamoDeployPath')
$weaselRoot = [string](Get-RegValue $hklm64 $brandSubkey 'WeaselRoot')
$inprocPath = [string](Get-RegValue $hklm64 "SOFTWARE\Classes\CLSID\$clsid\InProcServer32" '')

$health = Measure-Step {
  if (-not (Test-Path -LiteralPath $healthScript)) { throw "missing $healthScript" }
  $raw = & powershell -NoProfile -ExecutionPolicy Bypass -File $healthScript -Json
  $raw | ConvertFrom-Json
}
$tsfAudit = Measure-Step {
  if (-not (Test-Path -LiteralPath $tsfScript)) { throw "missing $tsfScript" }
  $raw = & powershell -NoProfile -ExecutionPolicy Bypass -File $tsfScript -Json
  $raw | ConvertFrom-Json
}
$pipe = Test-PipeTiming $pipeName

$serverProcesses = @(Get-Process -Name 'FamoRuntime' -ErrorAction SilentlyContinue | ForEach-Object {
  $procPath = $null
  $procStartTimeUtc = $null
  try { $procPath = $_.Path } catch { $procPath = $null }
  try { $procStartTimeUtc = $_.StartTime.ToUniversalTime().ToString('o') } catch { $procStartTimeUtc = $null }
  [pscustomobject]@{
    id = $_.Id
    path = $procPath
    startTimeUtc = $procStartTimeUtc
  }
})

$userTipHomes = New-Object System.Collections.Generic.List[string]
$expectedTip = "0804:$clsid$profileGuid"
$userProfileRoot = $hkcu.OpenSubKey('Control Panel\International\User Profile')
try {
  if ($userProfileRoot) {
    foreach ($lang in $userProfileRoot.GetSubKeyNames()) {
      $langKey = $userProfileRoot.OpenSubKey($lang)
      try {
        if ($langKey -and (@($langKey.GetValueNames()) | Where-Object { $_ -ieq $expectedTip })) {
          $userTipHomes.Add($lang)
        }
      } finally {
        if ($langKey) { $langKey.Close() }
      }
    }
  }
} finally {
  if ($userProfileRoot) { $userProfileRoot.Close() }
}

$healthState = $null
$panelProbe = $null
if ($health.ok -and $health.value) {
  $firstHealth = @($health.value | Select-Object -First 1)
  if ($firstHealth.Count -gt 0) {
    $healthState = $firstHealth[0].healthState
    $panelProbe = $firstHealth[0].panelProbe
  }
}

$timingTail = Read-LogTail $timingLog $RecentLogLines
$lastDeployTiming = @($timingTail | Where-Object { $_ -match 'component=deployQueue' } | Select-Object -Last 1)

$bundle = [pscustomobject]@{
  generatedAtUtc = (Get-Date).ToUniversalTime().ToString('o')
  probeMode = 'ReadOnly'
  privacy = [pscustomobject]@{
    collectsClipboard = $false
    collectsTypedText = $false
    collectsDictionaries = $false
    collectsSecrets = $false
    logRedaction = 'api_key/token/password/secret/sk-* patterns redacted from log tails'
  }
  install = [pscustomobject]@{
    installDir = $installDir
    serverExecutable = Get-FileVersionInfo $serverExe
    tsfDll = Get-FileVersionInfo $inprocPath
    deployer = Get-FileVersionInfo $deployerPath
  }
  registry = [pscustomobject]@{
    clsid = $clsid
    profileGuid = $profileGuid
    brandSubkey = "HKLM64\$brandSubkey"
    inprocServer32 = $inprocPath
    currentUserTipHomes = $userTipHomes.ToArray()
  }
  readiness = [pscustomobject]@{
    healthState = $healthState
    serverProcesses = $serverProcesses
    pipe = $pipe
  }
  timings = [pscustomobject]@{
    healthProbeMs = $health.elapsedMs
    tsfRegistrationAuditMs = $tsfAudit.elapsedMs
    ipcPipeConnectMs = $pipe.elapsedMs
    deployQueue = [pscustomobject]@{
      source = 'famo-timing.log when FAMO_LOCAL_TIMING=1'
      lastSample = $(if ($lastDeployTiming.Count -gt 0) { $lastDeployTiming[-1] } else { $null })
      rateLimited = $true
      boundedLogBytes = 131072
    }
    candidateStatusUi = [pscustomobject]@{
      source = 'panelProbe/manual PANEL smoke until automated window probe exists'
      panelProbe = $panelProbe
      layoutUpdateTiming = 'Manual:PANEL-panel-smoothness'
    }
  }
  health = [pscustomobject]@{
    ok = $health.ok
    elapsedMs = $health.elapsedMs
    error = $health.error
    rows = $health.value
  }
  tsfRegistration = [pscustomobject]@{
    ok = $tsfAudit.ok
    elapsedMs = $tsfAudit.elapsedMs
    error = $tsfAudit.error
    rows = $tsfAudit.value
  }
  dataIsolation = [pscustomobject]@{
    famoDir = $famoDir
    famoDirExists = Test-Path -LiteralPath $famoDir
    userWeaselDir = Join-Path $env:APPDATA 'Rime'
    userWeaselDirExists = Test-Path -LiteralPath (Join-Path $env:APPDATA 'Rime')
    brandReferencesUserWeaselDir = @($installDir, $serverExe, $deployerPath, $weaselRoot) -match '\\AppData\\Roaming\\Rime'
    proof = 'No user Weasel files are enumerated or hashed by this diagnostics script.'
  }
  logs = [pscustomobject]@{
    settingsLog = [pscustomobject]@{ path = $settingsLog; tail = Read-LogTail $settingsLog $RecentLogLines }
    timingLog = [pscustomobject]@{ path = $timingLog; tail = $timingTail }
  }
  concerns = [pscustomobject]@{
    ipc = $pipe
    runtimeHealth = $healthState
    deployQueue = $(if ($lastDeployTiming.Count -gt 0) { $lastDeployTiming[-1] } else { 'no opt-in deployQueue timing sample' })
    candidateStatusUi = $panelProbe
  }
}

if ($Json) {
  ConvertTo-Json -InputObject $bundle -Depth 8
} else {
  Write-Host "# Famo diagnostics"
  Write-Host ("generatedAtUtc: {0}" -f $bundle.generatedAtUtc)
  Write-Host ("healthState: {0} ({1} ms)" -f $bundle.readiness.healthState, $bundle.timings.healthProbeMs)
  Write-Host ("ipcPipe: {0} ({1} ms)" -f $bundle.readiness.pipe.state, $bundle.readiness.pipe.elapsedMs)
  Write-Host ("currentUserTipHomes: {0}" -f (($bundle.registry.currentUserTipHomes -join ', ')))
  Write-Host ("deployQueue timing: {0}" -f $bundle.concerns.deployQueue)
  Write-Host ("candidate/status UI: {0}" -f $bundle.concerns.candidateStatusUi)
}
