[CmdletBinding()]
param(
  [switch] $Json
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$identity = Get-Content (Join-Path $root 'famo-identity.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$clsid = '{' + ([string]$identity.guids.clsidTextService).ToUpperInvariant() + '}'
$profileGuid = '{' + ([string]$identity.guids.guidProfile).ToUpperInvariant() + '}'
$expectedTip = "0804:$clsid$profileGuid"
$brandKey = 'HKLM:\' + [string]$identity.registry.brandKey
$machineComKey = "HKLM:\Software\Classes\CLSID\$clsid\InProcServer32"
$userComKey = "HKCU:\Software\Classes\CLSID\$clsid\InProcServer32"
$tipKey = "HKLM:\Software\Microsoft\CTF\TIP\$clsid"
$probeMode = 'ReadOnly'
$userWeaselDataPolicy = 'NoWrite:%AppData%\Rime'
$results = New-Object System.Collections.Generic.List[object]

function Add-Audit {
  param(
    [string] $Id,
    [string] $Scope,
    [bool] $Pass,
    [string] $Detail,
    [string] $Expected,
    [string] $Actual
  )
  $results.Add([pscustomobject]@{
    id = $Id
    scope = $Scope
    pass = $Pass
    status = if ($Pass) { 'PASS' } else { 'FAIL' }
    detail = $Detail
    expected = $Expected
    actual = $Actual
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

$brandPresent = Test-Path -LiteralPath $brandKey
$machineComPresent = Test-Path -LiteralPath $machineComKey
$userComPresent = Test-Path -LiteralPath $userComKey
$tipPresent = Test-Path -LiteralPath $tipKey
$brand = if ($brandPresent) { Get-ItemProperty -LiteralPath $brandKey } else { $null }
$notInstalled = -not $brandPresent -and -not $machineComPresent -and -not $userComPresent -and -not $tipPresent
$installState = if ($brand) { [string]$brand.InstallState } else { 'NotInstalled' }
$isPending = $installState -eq 'PendingReboot'
$target = if ($isPending) { [string]$brand.PendingTarget } elseif ($brand) { [string]$brand.InstallDir } else { '' }
$profileTool = if ($isPending -and $target) { Join-Path $target 'FamoProfileTool.exe' } elseif ($brand) { [string]$brand.ProfileTool } else { '' }

$inprocDll = ''
$threadingModel = ''
if ($machineComPresent) {
  $com = Get-Item -LiteralPath $machineComKey
  $inprocDll = [string]$com.GetValue('')
  $threadingModel = [string]$com.GetValue('ThreadingModel')
}
$expectedDll = if ($target) { Join-Path $target 'FamoTextService.dll' } else { '' }
$comOk = $machineComPresent -and -not $userComPresent -and (Same-Path $inprocDll $expectedDll) -and $threadingModel -eq 'Apartment'
Add-Audit 'TSF-COM' 'HKLM COM registration; per-user COM override absent' ($notInstalled -or [bool]$comOk) `
  'The machine COM registration must point to the active transaction and no legacy HKCU key may shadow it.' `
  "HKLM COM=$expectedDll; ThreadingModel=Apartment; HKCU override absent" `
  $(if ($notInstalled) { 'absent with clean uninstall' } else { "COM=$inprocDll; ThreadingModel=$threadingModel; userOverride=$userComPresent" })

$profileOutput = ''
$profileExit = -1
if ($profileTool -and (Test-Path -LiteralPath $profileTool)) {
  $profileCommand = if ($isPending) { 'check-disabled' } else { 'check' }
  $profileOutput = (& $profileTool $profileCommand 2>&1 | Out-String).Trim()
  $profileExit = $LASTEXITCODE
}
Add-Audit 'TSF-PROFILE' 'profile registration' ($notInstalled -or $profileExit -eq 0) `
  'FamoProfileTool verifies registry, Simplified Chinese profile, expected enabled state, and keyboard category.' `
  $(if ($isPending) { 'registry=present profile=present enabled=no category=present' } else { 'registry=present profile=present enabled=yes category=present' }) `
  $(if ($notInstalled) { 'absent with clean uninstall' } else { "command=$profileCommand; exit=$profileExit; $profileOutput" })

$profileActive = $false
if ($profileTool -and (Test-Path -LiteralPath $profileTool)) {
  & $profileTool is-active *> $null
  $profileActive = $LASTEXITCODE -eq 0
}
$activeStateOk = $notInstalled -or -not $isPending -or -not $profileActive
Add-Audit 'TSF-ACTIVE' 'current profile' $activeStateOk `
  'PendingReboot must be inactive; Ready activation is best-effort because a background installer cannot select the input method for every foreground process.' `
  $(if ($isPending) { 'FamoProfileTool is-active exits 1' } else { 'registered input source is available via Win+Space; active may be yes or no' }) `
  $(if ($notInstalled) { 'N/A while not installed' } else { "active=$profileActive" })

$userTipHomes = New-Object System.Collections.Generic.List[string]
$userProfileRoot = Get-Item 'HKCU:\Control Panel\International\User Profile' -ErrorAction SilentlyContinue
if ($userProfileRoot) {
  foreach ($language in @($userProfileRoot.GetSubKeyNames())) {
    $languageKey = $userProfileRoot.OpenSubKey($language)
    try {
      if ($languageKey -and (@($languageKey.GetValueNames()) | Where-Object { $_ -ieq $expectedTip })) {
        $userTipHomes.Add($language)
      }
    } finally {
      if ($languageKey) { $languageKey.Close() }
    }
  }
}
$currentUserTipOk = if ($notInstalled -or $isPending) { $userTipHomes.Count -eq 0 } else { $userTipHomes.Count -gt 0 }
Add-Audit 'TSF-CURRENT-USER-TIP' 'Win+Space visibility' $currentUserTipOk `
  'The current-user input-source list is the automated proxy for Win+Space visibility.' `
  $(if ($isPending) { "value name $expectedTip absent while PendingReboot" } else { "value name $expectedTip under HKCU user profile" }) `
  $(if (($notInstalled -or $isPending) -and $userTipHomes.Count -eq 0) { 'absent as required' } elseif ($userTipHomes.Count) { "present under $($userTipHomes -join ', ')" } else { 'missing' })

$devCom = Test-Path 'HKCU:\Software\Classes\CLSID\{A6E6F585-4C92-459D-8D5B-175559605FB9}'
$identityOk = $notInstalled -or ($brand.Identity -eq 'Stable' -and -not $devCom)
Add-Audit 'TSF-IDENTITY' 'stable/development isolation' ([bool]$identityOk) `
  'The stable installer uses product GUIDs and does not leave the development COM identity registered.' `
  "stable=$clsid; development absent" `
  $(if ($notInstalled) { 'both identities absent with clean uninstall' } else { "Identity=$($brand.Identity); developmentCOM=$devCom" })

if ($Json) {
  ConvertTo-Json -InputObject $results.ToArray() -Depth 4
} else {
  foreach ($result in $results) {
    Write-Host ('[{0}] {1} ({2}) {3}' -f $result.id, $result.status, $result.scope, $result.detail)
    if (-not $result.pass) {
      Write-Host ('  expected: {0}' -f $result.expected)
      Write-Host ('  actual:   {0}' -f $result.actual)
    }
  }
  $failed = @($results | Where-Object { -not $_.pass }).Count
  Write-Host ('-- {0}/{1} TSF registration audit rows passed --' -f ($results.Count - $failed), $results.Count)
}

$failedRows = @($results | Where-Object { -not $_.pass })
if ($failedRows.Count -gt 0) { exit 1 } else { exit 0 }
