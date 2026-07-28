param(
  [string]$PatchPath = (Join-Path $PSScriptRoot '..\features\engine-abi.patch')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $PatchPath)) {
  throw "Missing patch: $PatchPath"
}

$forbidden = [regex]@'
TODO\(abi,target\)|handled/eaten|PREVIEW_ALL.*parity|EngineBackend|m_backend|ActiveBackend.*legacy|rime_api->(?:initialize|finalize|create_session|destroy_session|find_session|process_key|select_candidate_on_current_page|highlight_candidate_on_current_page|change_page|commit_composition|clear_composition|get_commit|get_status|get_context|free_commit|free_status|free_context|get_property|set_property|set_option|select_schema|get_option|get_state_label)
'@

$hits = New-Object System.Collections.Generic.List[string]
$lineNo = 0
Get-Content -LiteralPath $PatchPath | ForEach-Object {
  $lineNo++
  $line = $_
  if ($line.StartsWith('+') -and -not $line.StartsWith('+++') -and
      $forbidden.IsMatch($line)) {
    $hits.Add("${lineNo}: $line")
  }
}

if ($hits.Count) {
  Write-Error ("engine-abi.patch reintroduces legacy/direct-rime code or ABI parity blockers:`n" +
               ($hits -join "`n"))
  exit 1
}

$patchText = Get-Content -LiteralPath $PatchPath -Raw
$processKeyHunk = [regex]::Match(
  $patchText,
  '(?ms)^@@[^\r\n]*RimeWithWeaselHandler::ProcessKeyEvent[^\r\n]*\r?\n(?<body>.*?)(?=^@@|\z)')
if (-not $processKeyHunk.Success) {
  throw 'engine-abi.patch must contain the ProcessKeyEvent ABI routing hunk.'
}

$processKeyAdded = (($processKeyHunk.Groups['body'].Value -split '\r?\n') |
  Where-Object { $_.StartsWith('+') -and -not $_.StartsWith('+++') } |
  ForEach-Object { $_.Substring(1) }) -join "`n"
$handledAssignments = [regex]::Matches(
  $processKeyAdded,
  '(?ms)^\s*handled\s*=\s*(?<expression>.*?);')
if ($handledAssignments.Count -ne 1) {
  throw ('ProcessKeyEvent must assign handled exactly once after initialization; ' +
         'a later overwrite would discard the ABI handled/eaten result.')
}
$handledAssignment = $handledAssignments[0]

$handledExpression = $handledAssignment.Groups['expression'].Value
$expectedHandledExpression =
  '^\s*\(\s*m_abi_view_valid\s*&&\s*\(\s*m_abi_view\.state_flags\s*&\s*FAMO_COMPOSITION_HANDLED\s*\)\s*\)\s*\?\s*True\s*:\s*False\s*$'
if (-not [regex]::IsMatch($handledExpression, $expectedHandledExpression)) {
  throw ('ProcessKeyEvent must return eaten for an empty view with FAMO_COMPOSITION_HANDLED, ' +
         'and pass through an empty view without it; read state_flags directly instead of inferring handled from view content.')
}
if ($handledExpression -match
    'FAMO_COMPOSITION_HAS_(?:PREEDIT|COMMIT|CANDIDATES)') {
  throw 'ProcessKeyEvent handled/eaten must not depend on preedit, commit, or candidate presence.'
}

function Get-ResultingHunkText {
  param(
    [Parameter(Mandatory = $true)] [string] $Patch,
    [Parameter(Mandatory = $true)] [string] $FunctionName
  )

  $hunks = [regex]::Matches(
    $Patch,
    '(?ms)^@@[^\r\n]*\r?\n(?<body>.*?)(?=^@@|^diff --git|\z)')
  $hunk = $hunks |
    Where-Object {
      ($_.Value -split '\r?\n', 2)[0].Contains($FunctionName)
    } |
    Select-Object -First 1
  if ($null -eq $hunk) {
    $hunk = $hunks |
      Where-Object { $_.Groups['body'].Value.Contains($FunctionName) } |
      Select-Object -First 1
  }
  if ($null -eq $hunk) {
    throw "engine-abi.patch must contain the $FunctionName hunk."
  }

  return (($hunk.Groups['body'].Value -split '\r?\n') |
    Where-Object {
      ($_.StartsWith('+') -and -not $_.StartsWith('+++')) -or
      $_.StartsWith(' ')
    } |
    ForEach-Object { $_.Substring(1) }) -join "`n"
}

$addSessionResulting = Get-ResultingHunkText -Patch $patchText -FunctionName 'RimeWithWeaselHandler::AddSession'
$createFailureGuard = [regex]::Match(
  $addSessionResulting,
  '(?ms)const\s+int32_t\s+create_context_result\s*=\s*m_engine_host\.api\(\)\.create_context\(.*?;\s*' +
  'if\s*\(\s*create_context_result\s*!=\s*FAMO_ENGINE_OK\s*\|\|\s*!engine_ctx\s*\)\s*\{.*?return\s+0\s*;\s*\}')
if (-not $createFailureGuard.Success) {
  throw 'AddSession must observe create_context failure (including a null result) and return session id 0.'
}

$createGuardIndex = $addSessionResulting.IndexOf($createFailureGuard.Value)
$sessionRegistrationIndex = $addSessionResulting.IndexOf('_GenerateNewWeaselSessionId')
if ($sessionRegistrationIndex -lt 0 -or $createGuardIndex -gt $sessionRegistrationIndex) {
  throw 'AddSession must reject context creation failure before generating or registering a session id.'
}

$initialStatusFailureGuard = [regex]::Match(
  $addSessionResulting,
  '(?ms)if\s*\(\s*!_RefillAbiView\(engine_ctx\)\s*\)\s*\{\s*' +
  'm_engine_host\.api\(\)\.destroy_context\(engine_ctx\)\s*;\s*.*?return\s+0\s*;\s*\}')
if (-not $initialStatusFailureGuard.Success -or
    $addSessionResulting.IndexOf($initialStatusFailureGuard.Value) -gt $sessionRegistrationIndex) {
  throw 'AddSession must validate a new context and destroy/reject it before registering the session when status is unavailable.'
}

$selectSchemaResulting = Get-ResultingHunkText -Patch $patchText -FunctionName 'RimeWithWeaselHandler::SelectSchema'
if ($selectSchemaResulting.Contains('pair.second.engine_ctx = nullptr')) {
  throw 'SelectSchema must never clear the live context before a replacement is ready.'
}

$prepareLoopIndex = $selectSchemaResulting.IndexOf('for (auto& pair : m_session_status_map)')
$stageIndex = $selectSchemaResulting.IndexOf('replacements.push_back')
$commitLoopIndex = if ($stageIndex -ge 0) {
  $selectSchemaResulting.IndexOf('for (auto& replacement : replacements)', $stageIndex)
} else {
  -1
}
$swapIndex = $selectSchemaResulting.IndexOf('session_status.engine_ctx = replacement.context')
$destroyOldIndex = $selectSchemaResulting.IndexOf('destroy_context(old_ctx)')
if ($prepareLoopIndex -lt 0 -or $stageIndex -lt 0 -or $commitLoopIndex -lt 0 -or
    $swapIndex -lt 0 -or $destroyOldIndex -lt 0 -or
    -not ($prepareLoopIndex -lt $stageIndex -and
          $stageIndex -lt $commitLoopIndex -and
          $commitLoopIndex -lt $swapIndex -and
          $swapIndex -lt $destroyOldIndex)) {
  throw 'SelectSchema must stage every validated replacement, then swap it in before destroying the old context.'
}

$switchFailureCleanup = [regex]::Match(
  $selectSchemaResulting,
  '(?ms)if\s*\(\s*create_context_result\s*!=\s*FAMO_ENGINE_OK\s*\|\|\s*!new_ctx\s*\)\s*\{.*?' +
  'for\s*\(\s*auto&\s+replacement\s*:\s*replacements\s*\).*?' +
  'destroy_context\(replacement\.context\).*?return\s*;')
$switchValidationCleanup = [regex]::Match(
  $selectSchemaResulting,
  '(?ms)if\s*\(\s*!_RefillAbiView\(new_ctx\)\s*\)\s*\{.*?' +
  'destroy_context\(new_ctx\).*?' +
  'for\s*\(\s*auto&\s+replacement\s*:\s*replacements\s*\).*?' +
  'destroy_context\(replacement\.context\).*?return\s*;')
if (-not $switchFailureCleanup.Success -or -not $switchValidationCleanup.Success -or
    $selectSchemaResulting.IndexOf($switchFailureCleanup.Value) -gt $commitLoopIndex -or
    $selectSchemaResulting.IndexOf($switchValidationCleanup.Value) -gt $commitLoopIndex) {
  throw 'SelectSchema must clean up staged replacements and return before commit on creation or validation failure.'
}

Write-Host "PASS: engine-abi.patch added lines are ABI-only."
