[CmdletBinding()]
param(
  [ValidateSet('current', 'microsoft-pinyin', 'famo')]
  [string] $Label = 'current',
  [ValidateRange(1, 3600)]
  [int] $DurationSeconds = 30,
  [ValidateRange(10, 1000)]
  [int] $PressureIntervalMs = 40,
  [string] $EvidenceDirectory = (Join-Path $env:LOCALAPPDATA 'Famo\diagnostics\explorer-hang-probes'),
  [switch] $MonitorOnly,
  [switch] $GeneratePressure,
  [switch] $CaptureDumpOnHang,
  [switch] $SelfCheck
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$FamoTip = '0804:{54EAD76A-B864-4A6D-9C82-148E3352BEE7}{0158C2BA-4E96-4BA8-B505-E1BBEBB3FA33}'
$MicrosoftPinyinTip = '0804:{81D4E9C9-1D3B-41BC-9E6C-4B40BF79E35E}{FA550B04-5AD7-411F-A5AC-CA038EC515D7}'
$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..'))
$HealthScript = Join-Path $PSScriptRoot '..\..\weasel-fork\tests\Test-FamoHealth.ps1'

function Get-IsoUtc {
  param([datetime] $Value = (Get-Date))
  return $Value.ToUniversalTime().ToString('o')
}

function Get-OptionalRegistryValue {
  param(
    [Parameter(Mandatory = $true)][string] $Path,
    [Parameter(Mandatory = $true)][string] $Name
  )
  if (-not (Test-Path -LiteralPath $Path)) { return $null }
  $item = Get-ItemProperty -LiteralPath $Path -ErrorAction Stop
  $property = $item.PSObject.Properties[$Name]
  if ($null -eq $property) { return $null }
  return $property.Value
}

function Get-BootTime {
  try {
    return (Get-CimInstance Win32_OperatingSystem -ErrorAction Stop).LastBootUpTime
  } catch {
    if (-not ('FamoKernelUptime' -as [type])) {
      Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class FamoKernelUptime {
  [DllImport("kernel32.dll")]
  public static extern ulong GetTickCount64();
}
'@
    }
    return (Get-Date).AddMilliseconds(-[double][FamoKernelUptime]::GetTickCount64())
  }
}

function Select-ExplorerForSession {
  param(
    [object[]] $Processes,
    [int] $SessionId
  )
  $matches = @($Processes |
    Where-Object { $_.SessionId -eq $SessionId } |
    Sort-Object StartTime)
  if ($matches.Count -eq 0) { return $null }
  return $matches[0]
}

function Resolve-ProbeVerdict {
  param(
    [int] $EventCount,
    [bool] $HadNonResponsive,
    [bool] $ProcessChanged,
    [bool] $Completed
  )

  if ($EventCount -gt 0) { return 'red' }
  if ($HadNonResponsive -and $ProcessChanged) { return 'red' }
  if ($HadNonResponsive -or $ProcessChanged -or -not $Completed) { return 'inconclusive' }
  return 'green'
}

function Invoke-SelfCheck {
  $missingOverride = Get-OptionalRegistryValue `
    -Path 'HKCU:\Control Panel\International\User Profile' `
    -Name '__FamoExplorerProbeMissingValue__'
  $bootTime = Get-BootTime
  $fallbackExplorer = Select-ExplorerForSession -Processes @(
    [pscustomobject]@{ Id = 3; SessionId = 2; StartTime = [datetime]'2026-01-02' },
    [pscustomobject]@{ Id = 2; SessionId = 1; StartTime = [datetime]'2026-01-02' },
    [pscustomobject]@{ Id = 1; SessionId = 1; StartTime = [datetime]'2026-01-01' }
  ) -SessionId 1
  $cases = @(
    @{ name = 'event-is-red'; expected = 'red'; actual = Resolve-ProbeVerdict 1 $false $false $true },
    @{ name = 'hang-restart-is-red'; expected = 'red'; actual = Resolve-ProbeVerdict 0 $true $true $true },
    @{ name = 'short-run-is-inconclusive'; expected = 'inconclusive'; actual = Resolve-ProbeVerdict 0 $false $false $false },
    @{ name = 'restart-without-hang-is-inconclusive'; expected = 'inconclusive'; actual = Resolve-ProbeVerdict 0 $false $true $true },
    @{ name = 'full-clean-run-is-green'; expected = 'green'; actual = Resolve-ProbeVerdict 0 $false $false $true },
    @{ name = 'missing-optional-registry-value-is-null'; expected = $true; actual = ($null -eq $missingOverride) },
    @{ name = 'boot-time-has-bounded-fallback'; expected = $true; actual = ($bootTime -is [datetime] -and $bootTime -lt (Get-Date).AddSeconds(-1)) },
    @{ name = 'explorer-fallback-is-current-session-oldest'; expected = 1; actual = $fallbackExplorer.Id }
  )

  $failures = @($cases | Where-Object { $_.actual -ne $_.expected })
  $result = [pscustomobject]@{
    mode = 'SelfCheck'
    passed = ($failures.Count -eq 0)
    cases = $cases
  }
  $result | ConvertTo-Json -Depth 5
  if ($failures.Count -gt 0) { exit 1 }
  exit 0
}

if ($SelfCheck) { Invoke-SelfCheck }
if ($MonitorOnly -and $GeneratePressure) {
  throw '-MonitorOnly and -GeneratePressure are mutually exclusive.'
}

function Initialize-NativeProbe {
  if ('FamoExplorerNativeProbe' -as [type]) { return }
  $source = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class FamoExplorerNativeProbe {
  enum WctObjectType { None=0, CriticalSection, SendMessage, Mutex, Alpc, Com, ThreadWait, ProcessWait, Thread, ComActivation, Unknown, SocketIo, SmbIo, Max }
  enum WctObjectStatus { None=0, NoAccess, Running, Blocked, PidOnly, PidOnlyRpcss, Owned, NotOwned, Abandoned, Unknown, Error, Max }

  [StructLayout(LayoutKind.Explicit, Size=272, CharSet=CharSet.Unicode)]
  unsafe struct NodeUnion {
    [FieldOffset(0)] public fixed char ObjectName[128];
    [FieldOffset(256)] public long Timeout;
    [FieldOffset(264)] public int Alertable;
    [FieldOffset(0)] public uint ProcessId;
    [FieldOffset(4)] public uint ThreadId;
    [FieldOffset(8)] public uint WaitTime;
    [FieldOffset(12)] public uint ContextSwitches;
    public string Name { get { fixed(char* p=ObjectName) return new string(p); } }
  }

  [StructLayout(LayoutKind.Sequential, Size=280, CharSet=CharSet.Unicode)]
  unsafe struct Node {
    public WctObjectType ObjectType;
    public WctObjectStatus ObjectStatus;
    public NodeUnion Data;
  }

  [DllImport("user32.dll")] static extern IntPtr GetShellWindow();
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
  [DllImport("advapi32.dll", SetLastError=true)] static extern IntPtr OpenThreadWaitChainSession(uint flags, IntPtr callback);
  [DllImport("advapi32.dll")] static extern void CloseThreadWaitChainSession(IntPtr handle);
  [DllImport("advapi32.dll", SetLastError=true)] static extern bool GetThreadWaitChain(IntPtr handle, IntPtr context, uint flags, uint threadId, ref uint count, [In,Out] Node[] nodes, out int cycle);

  public static uint ShellProcessId() {
    uint processId;
    GetWindowThreadProcessId(GetShellWindow(), out processId);
    return processId;
  }

  public static string SampleWaitChain(uint threadId) {
    IntPtr handle=OpenThreadWaitChainSession(0, IntPtr.Zero);
    if(handle==IntPtr.Zero) throw new Exception("OpenThreadWaitChainSession failed: "+Marshal.GetLastWin32Error());
    try {
      uint count=16;
      var nodes=new Node[16];
      int cycle;
      if(!GetThreadWaitChain(handle, IntPtr.Zero, 0x7, threadId, ref count, nodes, out cycle))
        return "error:"+Marshal.GetLastWin32Error();
      var values=new List<string>();
      for(int i=0;i<count;i++) {
        var node=nodes[i];
        if(node.ObjectType==WctObjectType.Thread)
          values.Add(node.ObjectType+":"+node.ObjectStatus+":"+node.Data.ProcessId+":"+node.Data.ThreadId);
        else
          values.Add(node.ObjectType+":"+node.ObjectStatus+":"+node.Data.Name);
      }
      if(cycle!=0) values.Add("cycle:true");
      return string.Join("|", values);
    } finally { CloseThreadWaitChainSession(handle); }
  }
}
'@
  if ((Get-Command Add-Type).Parameters.ContainsKey('CompilerOptions')) {
    Add-Type -TypeDefinition $source -Language CSharp -CompilerOptions '/unsafe'
  } else {
    $compiler = New-Object System.CodeDom.Compiler.CompilerParameters
    $compiler.CompilerOptions = '/unsafe'
    $compiler.GenerateInMemory = $true
    Add-Type -TypeDefinition $source -Language CSharp -CompilerParameters $compiler
  }
}

function Get-ShellExplorerProcess {
  $id = [int][FamoExplorerNativeProbe]::ShellProcessId()
  if ($id -gt 0) {
    $shell = Get-Process -Id $id -ErrorAction SilentlyContinue
    if ($shell) { return $shell }
  }
  $sessionId = [System.Diagnostics.Process]::GetCurrentProcess().SessionId
  return Select-ExplorerForSession -Processes @(Get-Process explorer -ErrorAction SilentlyContinue) -SessionId $sessionId
}

function Get-ImeModules {
  param([System.Diagnostics.Process] $Process)
  if (-not $Process) { return @() }
  try {
    return @($Process.Modules | Where-Object { $_.ModuleName -match '(?i)Famo|Weasel|Rime' } | ForEach-Object {
      $hash = $null
      try { $hash = (Get-FileHash -LiteralPath $_.FileName -Algorithm SHA256).Hash } catch { }
      [pscustomobject]@{ name = $_.ModuleName; path = $_.FileName; sha256 = $hash }
    })
  } catch {
    return @([pscustomobject]@{ name = $null; path = $null; sha256 = $null; error = $_.Exception.Message })
  }
}

function Get-ExplorerSnapshot {
  param([switch] $IncludeModules)
  $process = Get-ShellExplorerProcess
  if (-not $process) {
    return [pscustomobject]@{ atUtc = Get-IsoUtc; present = $false; pid = $null; startUtc = $null; responding = $false; imeModules = @() }
  }
  $responding = $false
  try { $responding = $process.Responding } catch { }
  return [pscustomobject]@{
    atUtc = Get-IsoUtc
    present = $true
    pid = $process.Id
    startUtc = Get-IsoUtc $process.StartTime
    responding = $responding
    imeModules = $(if ($IncludeModules) { @(Get-ImeModules $process) } else { @() })
  }
}

function Get-InputState {
  $languages = New-Object System.Collections.Generic.List[object]
  foreach ($language in (Get-WinUserLanguageList)) {
    $languages.Add([pscustomobject]@{ languageTag = $language.LanguageTag; inputMethodTips = @($language.InputMethodTips) })
  }
  return [pscustomobject]@{
    defaultOverride = [string](Get-OptionalRegistryValue -Path 'HKCU:\Control Panel\International\User Profile' -Name InputMethodOverride)
    languages = $languages.ToArray()
  }
}

function Get-FamoHealthSnapshot {
  if (-not (Test-Path -LiteralPath $HealthScript)) {
    return [pscustomobject]@{ available = $false; error = "missing $HealthScript" }
  }
  try {
    $raw = & powershell -NoProfile -ExecutionPolicy Bypass -File $HealthScript -Json
    $parsed = $raw | ConvertFrom-Json
    $rows = New-Object System.Collections.Generic.List[object]
    foreach ($row in $parsed) { $rows.Add($row) }
    return [pscustomobject]@{ available = $true; rows = $rows.ToArray() }
  } catch {
    return [pscustomobject]@{ available = $false; error = $_.Exception.Message }
  }
}

function Get-ExplorerHangEvents {
  param([datetime] $StartTime, [long] $AfterRecordId)
  $events = @(Get-WinEvent -FilterHashtable @{ LogName='Application'; StartTime=$StartTime; Id=1001,1002 } -ErrorAction SilentlyContinue |
    Where-Object { $_.RecordId -gt $AfterRecordId -and $_.Message -match '(?i)explorer\.exe' -and ($_.Id -eq 1002 -or $_.Message -match 'AppHangB1') })
  return @($events | ForEach-Object {
    $reportId = $null
    if ($_.Message -match '(?im)(?:Report ID|Report Id|\u62a5\u544a ID):\s*([^\r\n]+)') { $reportId = $Matches[1].Trim() }
    [pscustomobject]@{
      atUtc = Get-IsoUtc $_.TimeCreated
      id = $_.Id
      provider = $_.ProviderName
      recordId = $_.RecordId
      eventName = $(if ($_.Message -match 'AppHangB1') { 'AppHangB1' } else { 'ApplicationHang' })
      application = 'explorer.exe'
      reportId = $reportId
    }
  })
}

function Get-WaitChains {
  param([int] $ProcessId)
  $process = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
  if (-not $process) { return @() }
  return @($process.Threads | ForEach-Object {
    $chain = $null
    try { $chain = [FamoExplorerNativeProbe]::SampleWaitChain([uint32]$_.Id) } catch { $chain = 'error:' + $_.Exception.Message }
    [pscustomobject]@{ threadId = $_.Id; chain = $chain }
  } | Where-Object { $_.chain -and $_.chain -notmatch '^Thread:Running' })
}

function Save-HangDump {
  param([int] $ProcessId, [string] $Directory)
  $path = Join-Path $Directory ("explorer-{0}-{1}.dmp" -f $ProcessId, (Get-Date -Format 'yyyyMMdd-HHmmss'))
  $runner = Join-Path $env:SystemRoot 'System32\rundll32.exe'
  $comsvcs = Join-Path $env:SystemRoot 'System32\comsvcs.dll'
  try {
    $arguments = '"{0}", MiniDump {1} "{2}" full' -f $comsvcs, $ProcessId, $path
    $collector = Start-Process -FilePath $runner -ArgumentList $arguments -PassThru -WindowStyle Hidden
    if (-not $collector.WaitForExit(30000)) {
      Stop-Process -Id $collector.Id -Force -ErrorAction SilentlyContinue
      return [pscustomobject]@{ path = $path; captured = $false; error = 'collector timed out after 30 seconds' }
    }
    if (-not (Test-Path -LiteralPath $path)) {
      return [pscustomobject]@{ path = $path; captured = $false; error = "collector exit $($collector.ExitCode); dump missing" }
    }
    return [pscustomobject]@{ path = $path; captured = $true; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash; length = (Get-Item -LiteralPath $path).Length }
  } catch {
    return [pscustomobject]@{ path = $path; captured = $false; error = $_.Exception.Message }
  }
}

function Test-ProfileEvidence {
  param([string] $RunLabel, $InputState, $Explorer)
  $hasFamo = @($Explorer.imeModules | Where-Object { $_.name -ieq 'FamoTsf.dll' }).Count -gt 0
  if ($RunLabel -eq 'famo') {
    return [pscustomobject]@{ pass = ($InputState.defaultOverride -ieq $FamoTip -and $hasFamo); expectedTip = $FamoTip; famoModuleExpected = $true; famoModuleLoaded = $hasFamo }
  }
  if ($RunLabel -eq 'microsoft-pinyin') {
    return [pscustomobject]@{ pass = ($InputState.defaultOverride -ieq $MicrosoftPinyinTip -and -not $hasFamo); expectedTip = $MicrosoftPinyinTip; famoModuleExpected = $false; famoModuleLoaded = $hasFamo }
  }
  return [pscustomobject]@{ pass = $true; expectedTip = $null; famoModuleExpected = $null; famoModuleLoaded = $hasFamo }
}

Initialize-NativeProbe

$evidenceFull = [System.IO.Path]::GetFullPath($EvidenceDirectory)
$repoPrefix = $RepoRoot.TrimEnd('\') + '\'
if ($evidenceFull.TrimEnd('\') -ieq $RepoRoot.TrimEnd('\') -or $evidenceFull.StartsWith($repoPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
  throw 'EvidenceDirectory must be outside the repository.'
}
New-Item -ItemType Directory -Force -Path $evidenceFull | Out-Null

$marker = Get-Date
$recordBaseline = 0L
try { $recordBaseline = [long](Get-WinEvent -LogName Application -MaxEvents 1).RecordId } catch { }
$baseline = Get-ExplorerSnapshot -IncludeModules
$inputState = Get-InputState
$health = Get-FamoHealthSnapshot
$boot = Get-BootTime
$transitions = New-Object System.Collections.Generic.List[object]
$transitions.Add($baseline)
$waitChains = @()
$dump = $null
$hadNonResponsive = -not $baseline.responding
$processChanged = $false
$keyCount = 0
$pressureReady = $false
$pressureError = $null
$shell = $null
$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
$runError = $null

try {
  if ($GeneratePressure) {
    $target = Join-Path $evidenceFull 'pressure-target'
    New-Item -ItemType Directory -Force -Path $target | Out-Null
    Start-Process -FilePath explorer.exe -ArgumentList ('"{0}"' -f $target)
    Start-Sleep -Milliseconds 1200
    $shell = New-Object -ComObject WScript.Shell
    $current = Get-ShellExplorerProcess
    $pressureReady = [bool]$shell.AppActivate($current.Id)
    if ($pressureReady) {
      $shell.SendKeys('^e')
      Start-Sleep -Milliseconds 200
    } else {
      $pressureError = 'could not activate the Explorer pressure target'
    }
  }

  $lastState = "{0}|{1}|{2}" -f $baseline.pid, $baseline.startUtc, $baseline.responding
  $pattern = 'famoimeprobe'
  while ($stopwatch.Elapsed.TotalSeconds -lt $DurationSeconds) {
    if ($GeneratePressure -and $pressureReady) {
      $shell.SendKeys([string]$pattern[$keyCount % $pattern.Length])
      $keyCount++
      if (($keyCount % $pattern.Length) -eq 0) { $shell.SendKeys(' ') }
      if (($keyCount % ($pattern.Length * 8)) -eq 0) { $shell.SendKeys('^a{BACKSPACE}') }
    }

    $snapshot = Get-ExplorerSnapshot
    $state = "{0}|{1}|{2}" -f $snapshot.pid, $snapshot.startUtc, $snapshot.responding
    if ($state -ne $lastState) {
      $transitions.Add((Get-ExplorerSnapshot -IncludeModules))
      $lastState = $state
    }
    if ($snapshot.pid -ne $baseline.pid -or $snapshot.startUtc -ne $baseline.startUtc) { $processChanged = $true }
    if ($snapshot.present -and -not $snapshot.responding -and -not $hadNonResponsive) {
      $hadNonResponsive = $true
      $waitChains = @(Get-WaitChains $snapshot.pid)
      if ($CaptureDumpOnHang) { $dump = Save-HangDump $snapshot.pid $evidenceFull }
    }
    Start-Sleep -Milliseconds $PressureIntervalMs
  }
} catch {
  $runError = $_.Exception.Message
} finally {
  $stopwatch.Stop()
  if ($GeneratePressure -and $pressureReady) {
    try { $shell.SendKeys('{ESC}') } catch { }
  }
}

$end = Get-Date
$final = Get-ExplorerSnapshot -IncludeModules
$profileEvidence = Test-ProfileEvidence $Label $inputState $final
if ($final.pid -ne $baseline.pid -or $final.startUtc -ne $baseline.startUtc) { $processChanged = $true }
$events = @(Get-ExplorerHangEvents $marker $recordBaseline)
$completed = ($runError -eq $null -and $stopwatch.Elapsed.TotalSeconds -ge ($DurationSeconds - 0.1) -and (-not $GeneratePressure -or $pressureReady))
$signalVerdict = Resolve-ProbeVerdict $events.Count $hadNonResponsive $processChanged $completed
$verdict = if ($profileEvidence.pass -or $signalVerdict -eq 'red') { $signalVerdict } else { 'inconclusive' }
$runId = '{0}-{1}-{2}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $Label, ([guid]::NewGuid().ToString('N').Substring(0,8))
$outputPath = Join-Path $evidenceFull ($runId + '.json')

$result = [pscustomobject]@{
  schemaVersion = 1
  runId = $runId
  label = $Label
  mode = $(if ($MonitorOnly) { 'MonitorOnly' } elseif ($GeneratePressure) { 'FixedSyntheticPressure' } else { 'Monitor' })
  markerUtc = Get-IsoUtc $marker
  endUtc = Get-IsoUtc $end
  targetDurationSeconds = $DurationSeconds
  elapsedSeconds = [math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
  completed = $completed
  verdict = $verdict
  signalVerdict = $signalVerdict
  profileEvidence = $profileEvidence
  privacy = [pscustomobject]@{ collectsTypedText = $false; collectsClipboard = $false; collectsDictionaries = $false; collectsSecrets = $false; syntheticPatternId = 'ascii-probe-v1' }
  bootUtc = Get-IsoUtc $boot
  inputState = $inputState
  health = $health
  explorer = [pscustomobject]@{ baseline = $baseline; transitions = $transitions.ToArray(); final = $final; hadNonResponsive = $hadNonResponsive; processChanged = $processChanged }
  events = $events
  waitChains = $waitChains
  dump = $dump
  pressure = [pscustomobject]@{ requested = [bool]$GeneratePressure; targetActivated = $pressureReady; intervalMs = $PressureIntervalMs; keyCount = $keyCount; error = $pressureError }
  error = $runError
  evidencePath = $outputPath
}

$result | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $outputPath -Encoding UTF8
$result | ConvertTo-Json -Depth 10

if ($verdict -eq 'red') { exit 1 }
if ($verdict -eq 'inconclusive') { exit 2 }
exit 0
