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

Write-Host "PASS: engine-abi.patch added lines are ABI-only."
