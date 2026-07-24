[CmdletBinding()]
param(
  [switch] $Json
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$brandKey = 'HKLM:\Software\Famo\InputMethod'
$comKey = 'HKCU:\Software\Classes\CLSID\{54EAD76A-B864-4A6D-9C82-148E3352BEE7}\InprocServer32'
$tipKey = 'HKLM:\Software\Microsoft\CTF\TIP\{54EAD76A-B864-4A6D-9C82-148E3352BEE7}'
$runKey = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Run'
$runOnceKey = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\RunOnce'
$probeMode = 'ReadOnly'
$userWeaselDataPolicy = 'NoWrite:%AppData%\Rime'
$results = New-Object System.Collections.Generic.List[object]

function Add-Check {
  param([string] $Id, [string] $Severity, [bool] $Pass, [string] $Detail)
  $results.Add([pscustomobject]@{
    id = $Id
    severity = $Severity
    pass = $Pass
    detail = $Detail
    probeMode = $probeMode
    userWeaselDataPolicy = $userWeaselDataPolicy
  })
}

function Normalize-PathValue {
  param([string] $Path)
  if (-not $Path) { return '' }
  try { return [System.IO.Path]::GetFullPath($Path.Trim('"')).TrimEnd('\') }
  catch { return $Path.Trim('"').TrimEnd('\') }
}

function Same-Path {
  param([string] $Left, [string] $Right)
  return [string]::Equals(
    (Normalize-PathValue $Left),
    (Normalize-PathValue $Right),
    [System.StringComparison]::OrdinalIgnoreCase)
}

function Test-Manifest {
  param([string] $Target, [string] $Manifest)
  $problems = New-Object System.Collections.Generic.List[string]
  if (-not $Target -or -not (Test-Path -LiteralPath $Target)) {
    return [pscustomobject]@{ pass = $false; detail = 'active transaction target is missing' }
  }
  if (-not $Manifest -or -not (Test-Path -LiteralPath $Manifest)) {
    return [pscustomobject]@{ pass = $false; detail = 'active payload manifest is missing' }
  }

  $lines = @(Get-Content -LiteralPath $Manifest -Encoding UTF8)
  foreach ($header in @('format=1', 'product=Famo', 'protocol=1', 'architecture=x64', 'identity=Stable')) {
    if ($lines -notcontains $header) { $problems.Add("header:$header") }
  }
  $countLine = @($lines | Where-Object { $_ -match '^file_count=\d+$' } | Select-Object -First 1)
  $entries = @($lines | Where-Object { $_ -like 'file=*' })
  $declared = if ($countLine.Count) { [int]($countLine[0].Substring(11)) } else { -1 }
  if ($declared -ne $entries.Count) { $problems.Add("file_count:$declared/$($entries.Count)") }

  foreach ($line in $entries) {
    $parts = $line.Substring(5).Split('|')
    if ($parts.Count -ne 3 -or -not $parts[0] -or $parts[0] -match '(^[\\/]|\.\.|:)' -or $parts[2] -notmatch '^[0-9A-Fa-f]{64}$') {
      $problems.Add("entry:$line")
      continue
    }
    $file = Join-Path $Target $parts[0]
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
      $problems.Add("missing:$($parts[0])")
      continue
    }
    $item = Get-Item -LiteralPath $file
    if ($item.Length -ne [int64]$parts[1]) {
      $problems.Add("size:$($parts[0])")
      continue
    }
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $parts[2]) {
      $problems.Add("hash:$($parts[0])")
    }
  }
  $actual = @(Get-ChildItem -LiteralPath $Target -Recurse -File -Force).Count
  if ($actual -ne ($entries.Count + 1)) { $problems.Add("actual_count:$actual/$($entries.Count + 1)") }
  return [pscustomobject]@{
    pass = $problems.Count -eq 0
    detail = if ($problems.Count) { $problems -join '; ' } else { "manifest verified: $($entries.Count) files" }
  }
}

function Test-ControlPipe {
  $sid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
  $session = (Get-Process -Id $PID).SessionId
  $name = "Famo.Runtime.v1.$sid.$session.control-v1"
  $client = New-Object System.IO.Pipes.NamedPipeClientStream(
    '.', $name, [System.IO.Pipes.PipeDirection]::InOut, [System.IO.Pipes.PipeOptions]::Asynchronous)
  try {
    $client.Connect(200)
    return [pscustomobject]@{ state = 'Ready'; detail = "bounded secure control connect succeeded: $name" }
  } catch [System.TimeoutException] {
    return [pscustomobject]@{ state = 'Hung'; detail = "control pipe connect exceeded 200 ms: $name" }
  } catch {
    return [pscustomobject]@{ state = 'Broken'; detail = "control pipe connect failed: $($_.Exception.Message)" }
  } finally {
    $client.Dispose()
  }
}

$brandPresent = Test-Path -LiteralPath $brandKey
$comPresent = Test-Path -LiteralPath $comKey
$tipPresent = Test-Path -LiteralPath $tipKey
$runValue = [string](Get-ItemProperty -LiteralPath $runKey -Name FamoRuntime -ErrorAction SilentlyContinue).FamoRuntime
$resumeValue = [string](Get-ItemProperty -LiteralPath $runOnceKey -Name FamoResumePending -ErrorAction SilentlyContinue).FamoResumePending
$brand = if ($brandPresent) { Get-ItemProperty -LiteralPath $brandKey } else { $null }
$notInstalled = -not $brandPresent -and -not $comPresent -and -not $tipPresent -and -not $runValue -and -not $resumeValue
$installState = if ($brand) { [string]$brand.InstallState } else { 'NotInstalled' }
$isPending = $installState -eq 'PendingReboot'
$terminalStates = @('Ready', 'RolledBack', 'PendingReboot')

Add-Check 'H1' 'S0' ($notInstalled -or ($terminalStates -contains $installState)) `
  $(if ($notInstalled) { 'clean NotInstalled terminal state' } else { "InstallState=$installState" })

$version = if ($isPending) { [string]$brand.PendingVersion } else { [string]$brand.ActiveVersion }
$identityOk = $brand -and $brand.Identity -eq 'Stable' -and $version -and $brand.TransactionId -and
  (-not $isPending -or [string]$brand.PendingReason)
Add-Check 'H2' 'S0' ($notInstalled -or [bool]$identityOk) `
  $(if ($notInstalled) { 'identity N/A while not installed' } else { "Identity=$($brand.Identity); version=$version; transaction=$($brand.TransactionId); reason=$($brand.PendingReason)" })

$target = if ($isPending) { [string]$brand.PendingTarget } elseif ($brand) { [string]$brand.InstallDir } else { '' }
$server = if ($isPending -and $target) { Join-Path $target 'FamoRuntime.exe' } elseif ($brand) { [string]$brand.ServerExecutable } else { '' }
$profileTool = if ($isPending -and $target) { Join-Path $target 'FamoProfileTool.exe' } elseif ($brand) { [string]$brand.ProfileTool } else { '' }
$manifest = if ($isPending) { [string]$brand.PendingManifest } elseif ($brand) { [string]$brand.ActiveManifest } else { '' }
$pathsOk = $target -and (Test-Path -LiteralPath $target) -and
  (Same-Path $server (Join-Path $target 'FamoRuntime.exe')) -and
  (Same-Path $profileTool (Join-Path $target 'FamoProfileTool.exe')) -and
  (Same-Path $manifest (Join-Path $target 'payload-manifest.txt'))
Add-Check 'H3' 'S0' ($notInstalled -or [bool]$pathsOk) `
  $(if ($notInstalled) { 'transaction paths N/A while not installed' } else { "target=$target; server=$server; profileTool=$profileTool" })

$manifestResult = if ($notInstalled) {
  [pscustomobject]@{ pass = $true; detail = 'manifest N/A while not installed' }
} else { Test-Manifest $target $manifest }
Add-Check 'H3b' 'S0' ([bool]$manifestResult.pass) ([string]$manifestResult.detail)

$registeredDll = ''
$threadingModel = ''
if ($comPresent) {
  $com = Get-Item -LiteralPath $comKey
  $registeredDll = [string]$com.GetValue('')
  $threadingModel = [string]$com.GetValue('ThreadingModel')
}
$comOk = $comPresent -and (Same-Path $registeredDll (Join-Path $target 'FamoTextService.dll')) -and $threadingModel -eq 'Apartment'
Add-Check 'H4' 'S0' ($notInstalled -or [bool]$comOk) `
  $(if ($notInstalled) { 'HKCU COM override absent' } else { "COM=$registeredDll; ThreadingModel=$threadingModel" })

$profileOutput = ''
$profileExit = -1
if ($profileTool -and (Test-Path -LiteralPath $profileTool)) {
  $profileCommand = if ($isPending) { 'check-disabled' } else { 'check' }
  $profileOutput = (& $profileTool $profileCommand 2>&1 | Out-String).Trim()
  $profileExit = $LASTEXITCODE
}
Add-Check 'H5' 'S0' ($notInstalled -or $profileExit -eq 0) `
  $(if ($notInstalled) { 'TSF profile absent with clean uninstall' } else { "profile command=$profileCommand; exit=$profileExit; $profileOutput" })

$matchingProcess = $null
$processNotes = @()
foreach ($process in @(Get-Process -Name FamoRuntime -ErrorAction SilentlyContinue)) {
  $path = try { [string]$process.Path } catch { '' }
  $processNotes += if ($path) { "$($process.Id):$path" } else { "$($process.Id):(path unreadable)" }
  if ($path -and (Same-Path $path $server)) { $matchingProcess = $process }
}
$pipeProbe = if ($matchingProcess) { Test-ControlPipe } else { [pscustomobject]@{ state = 'Missing'; detail = 'exact FamoRuntime process is not running' } }
$runtimeOk = if ($isPending) {
  -not $matchingProcess -and $processNotes.Count -eq 0 -and -not $runValue
} else {
  $matchingProcess -and $pipeProbe.state -eq 'Ready'
}
Add-Check 'H6' $(if ($isPending) { 'S0' } else { 'S1' }) ($notInstalled -or [bool]$runtimeOk) `
  $(if ($notInstalled) { 'runtime and control pipe absent with clean uninstall' } elseif ($isPending) { "PendingReboot runtime and Run entry absent=$runtimeOk; processes=$($processNotes -join ', ')" } else { "$($pipeProbe.detail); processes=$($processNotes -join ', ')" })

$isolationProblems = @()
foreach ($value in @($target, $server, $profileTool, $manifest)) {
  if ($value -match '(?i)\\AppData\\Roaming\\Rime') { $isolationProblems += "Rime path:$value" }
}
if ($target -and (Test-Path -LiteralPath (Join-Path $target 'FamoDeploy.exe'))) { $isolationProblems += 'legacy FamoDeploy.exe present' }
if ($target -and (Test-Path -LiteralPath (Join-Path $target 'WinSparkle.dll'))) { $isolationProblems += 'legacy WinSparkle.dll present' }
Add-Check 'H7' 'S0' ($notInstalled -or $isolationProblems.Count -eq 0) `
  $(if ($isolationProblems.Count) { $isolationProblems -join '; ' } else { 'Stable native identity is isolated; NoWrite:%AppData%\Rime' })

$resourceProblems = @()
if (-not $notInstalled) {
  foreach ($relative in @(
    'FamoTextService.dll', 'FamoRuntime.exe', 'FamoRimeEngine.dll', 'FamoProfileTool.exe', 'rime.dll',
    'data\default.yaml', 'data\weasel.yaml', 'data\opencc',
    'settings\FamoSettings.exe', 'settings\FamoSettings.pri')) {
    if (-not (Test-Path -LiteralPath (Join-Path $target $relative))) { $resourceProblems += $relative }
  }
}
Add-Check 'H8' 'S1' ($notInstalled -or $resourceProblems.Count -eq 0) `
  $(if ($notInstalled) { 'active product resources N/A; a locked TSF host may await registered restart deletion' } elseif ($resourceProblems.Count) { "missing: $($resourceProblems -join ', ')" } else { 'native runtime, engine, data, and settings resources are present' })

$localFamo = Join-Path $env:LOCALAPPDATA 'Famo'
$resumeInstaller = if ($brand) { [string]$brand.ResumeInstaller } else { '' }
$firstRunReady = if ($isPending) {
  (-not $runValue) -and $resumeInstaller -and (Test-Path -LiteralPath $resumeInstaller) -and
    $resumeValue.IndexOf($resumeInstaller, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 -and
    $resumeValue.IndexOf("/FamoResume=$($brand.TransactionId)", [System.StringComparison]::OrdinalIgnoreCase) -ge 0
} else {
  (Same-Path $runValue $server) -and
    (Test-Path -LiteralPath (Join-Path $localFamo 'famo-settings.json')) -and
    (Test-Path -LiteralPath (Join-Path $localFamo 'build\default.yaml'))
}
Add-Check 'H9' $(if ($isPending) { 'S0' } else { 'S1' }) ($notInstalled -or [bool]$firstRunReady) `
  $(if ($notInstalled) { 'Run and RunOnce entries absent; retained user data is outside install health' } elseif ($isPending) { "Run absent; resume=$resumeInstaller; RunOnce=$resumeValue; coherent=$firstRunReady" } else { "Run=$runValue; first-run settings/build ready=$firstRunReady" })

function Check-Passed {
  param([string] $Id)
  $hit = @($results | Where-Object { $_.id -eq $Id } | Select-Object -First 1)
  return $hit.Count -gt 0 -and [bool]$hit[0].pass
}

$healthState = if ($notInstalled) {
  'NotInstalled'
} elseif ($installState -eq 'PendingReboot') {
  'PendingReboot'
} elseif (-not (Check-Passed 'H2') -or -not (Check-Passed 'H7')) {
  'WrongIdentity'
} elseif (-not (Check-Passed 'H1') -or -not (Check-Passed 'H3') -or -not (Check-Passed 'H3b') -or
        -not (Check-Passed 'H4') -or -not (Check-Passed 'H5')) {
  'Broken'
} elseif (-not (Check-Passed 'H6') -and $pipeProbe.state -eq 'Hung') {
  'Hung'
} elseif (-not (Check-Passed 'H6')) {
  'InstalledStopped'
} elseif ($installState -eq 'RolledBack') {
  'RolledBack'
} elseif (-not (Check-Passed 'H8') -or -not (Check-Passed 'H9')) {
  'Degraded'
} else {
  'Ready'
}

foreach ($result in $results) {
  $result | Add-Member -NotePropertyName healthState -NotePropertyValue $healthState -Force
}

if ($Json) {
  ConvertTo-Json -InputObject $results.ToArray() -Depth 4
} else {
  foreach ($result in $results) {
    $mark = if ($result.pass) { 'PASS' } else { 'FAIL' }
    Write-Host ('[{0}] {1} ({2}) {3}' -f $result.id, $mark, $result.severity, $result.detail)
  }
  $failed = @($results | Where-Object { -not $_.pass }).Count
  Write-Host ('[STATE] {0}' -f $healthState)
  Write-Host ('-- {0}/{1} checks passed --' -f ($results.Count - $failed), $results.Count)
}

$s0Failed = @($results | Where-Object { -not $_.pass -and $_.severity -eq 'S0' })
if ($s0Failed.Count -gt 0) { exit 1 } else { exit 0 }
