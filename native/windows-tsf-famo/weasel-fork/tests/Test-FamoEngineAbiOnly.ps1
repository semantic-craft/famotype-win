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

Write-Host "PASS: engine-abi.patch added lines are ABI-only."
