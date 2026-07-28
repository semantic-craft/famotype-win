param(
  [string]$PatchPath = (Join-Path $PSScriptRoot '..\features\engine-abi.patch')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $PatchPath)) {
  throw "Missing patch: $PatchPath"
}

$forbidden = [regex]@'
TODO\(abi,target\)|handled/eaten|PREVIEW_ALL.*parity|EngineBackend|m_backend|ActiveBackend.*legacy|m_engine_host\.api\(\)|m_engine_host\.Load\(|m_engine_host\.AbiRunnable\(|m_engine_host\.FreeView\(|m_abi_view|get_session_status\s*\(|m_session_status_map\s*\[|rime_api->(?:initialize|finalize|create_session|destroy_session|find_session|process_key|select_candidate_on_current_page|highlight_candidate_on_current_page|change_page|commit_composition|clear_composition|get_commit|get_status|get_context|free_commit|free_status|free_context|get_property|set_property|set_option|select_schema|get_option|get_state_label|set_notification_handler)
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
  '^\s*action_outcome\.handled\s*\?\s*True\s*:\s*False\s*$'
if (-not [regex]::IsMatch($handledExpression, $expectedHandledExpression)) {
  throw ('ProcessKeyEvent must use the recovery-aware ABI outcome as the ' +
         'single handled/eaten truth.')
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
  $matchingHunks = @($hunks |
    Where-Object {
      $_.Groups['body'].Value.Contains($FunctionName) -or
      ($_.Value -split '\r?\n', 2)[0].Contains($FunctionName)
    } |
    ForEach-Object { $_ })
  if ($matchingHunks.Count -eq 0) {
    throw "engine-abi.patch must contain the $FunctionName hunk."
  }

  return (($matchingHunks | ForEach-Object {
      $_.Groups['body'].Value -split '\r?\n'
    }) |
      Where-Object {
        ($_.StartsWith('+') -and -not $_.StartsWith('+++')) -or
        $_.StartsWith(' ')
      } |
      ForEach-Object { $_.Substring(1) }) -join "`n"
}

function Get-ResultingFileText {
  param(
    [Parameter(Mandatory = $true)] [string] $Patch,
    [Parameter(Mandatory = $true)] [string] $FilePath
  )

  $escapedPath = [regex]::Escape($FilePath)
  $fileDiff = [regex]::Match(
    $Patch,
    "(?ms)^diff --git a/$escapedPath b/$escapedPath\r?\n" +
    '(?<body>.*?)(?=^diff --git|\z)')
  if (-not $fileDiff.Success) {
    throw "engine-abi.patch must contain the $FilePath diff."
  }

  return (($fileDiff.Groups['body'].Value -split '\r?\n') |
    Where-Object {
      ($_.StartsWith('+') -and -not $_.StartsWith('+++')) -or
      $_.StartsWith(' ')
    } |
    ForEach-Object { $_.Substring(1) }) -join "`n"
}

$addSessionResulting = Get-ResultingHunkText -Patch $patchText -FunctionName 'RimeWithWeaselHandler::AddSession'
$createFailureGuard = [regex]::Match(
  $addSessionResulting,
  '(?ms)const\s+int32_t\s+create_context_result\s*=\s*m_engine_host\.CreateContext\(.*?;\s*' +
  'if\s*\(\s*create_context_result\s*!=\s*FAMO_ENGINE_OK\s*\|\|\s*!engine_ctx\s*\)\s*\{.*?return\s+0\s*;\s*\}')
if (-not $createFailureGuard.Success) {
  throw 'AddSession must observe create_context failure (including a null result) and return session id 0.'
}

$createGuardIndex = $addSessionResulting.IndexOf($createFailureGuard.Value)
$sessionRegistrationIndex = $addSessionResulting.IndexOf('_GenerateNewWeaselSessionId')
if ($sessionRegistrationIndex -lt 0 -or $createGuardIndex -gt $sessionRegistrationIndex) {
  throw 'AddSession must reject context creation failure before generating or registering a session id.'
}
if ($addSessionResulting.IndexOf('m_session_status_map.try_emplace(ipc_id)') -lt
      $sessionRegistrationIndex) {
  throw 'Only AddSession may create a session entry, and it must use try_emplace after generating the id.'
}

$initialStatusFailureGuards = [regex]::Matches(
  $addSessionResulting,
  '(?ms)if\s*\(\s*!_RefillAbiView\(engine_ctx\)\s*\)\s*\{\s*' +
  'm_session_status_map\.erase\(ipc_id\)\s*;\s*' +
  '_DestroyEngineContext\(engine_ctx\)\s*;\s*.*?return\s+0\s*;\s*\}')
if ($initialStatusFailureGuards.Count -ne 2) {
  throw 'AddSession must erase the provisional session and destroy its context on both preliminary and final STATUS failure.'
}

$globalInheritIndex = $addSessionResulting.IndexOf('if (m_global_ascii_mode)')
$famoOptionsIndex = $addSessionResulting.IndexOf('_ApplyFamoOptions(engine_ctx)')
$clientOptionsIndex = $addSessionResulting.IndexOf('_ReadClientInfo(ipc_id, buffer)')
$firstStatusIndex = $addSessionResulting.IndexOf('_RefillAbiView(engine_ctx)')
$schemaSettingsIndex = $addSessionResulting.IndexOf('_LoadSchemaSpecificSettings(ipc_id, schema_id)')
$appInlineIndex = $addSessionResulting.IndexOf('_ApplyAppInlinePreeditSetting(ipc_id)')
$inlineOptionIndex = $addSessionResulting.IndexOf('_UpdateInlinePreeditStatus(ipc_id)')
$finalStatusIndex = $addSessionResulting.LastIndexOf('_RefillAbiView(engine_ctx)')
$initialRespondIndex = $addSessionResulting.IndexOf('_Respond(ipc_id, eat)')
if (@($globalInheritIndex, $famoOptionsIndex, $clientOptionsIndex,
      $firstStatusIndex, $schemaSettingsIndex, $appInlineIndex,
      $inlineOptionIndex, $finalStatusIndex, $initialRespondIndex) |
    Where-Object { $_ -lt 0 }) {
  throw 'AddSession must contain the complete global/famo/app/schema/inline/final-STATUS sequence.'
}
if (-not ($globalInheritIndex -lt $famoOptionsIndex -and
          $famoOptionsIndex -lt $sessionRegistrationIndex -and
          $sessionRegistrationIndex -lt $clientOptionsIndex -and
          $clientOptionsIndex -lt $firstStatusIndex -and
          $firstStatusIndex -lt $schemaSettingsIndex -and
          $schemaSettingsIndex -lt $appInlineIndex -and
          $appInlineIndex -lt $inlineOptionIndex -and
          $inlineOptionIndex -lt $finalStatusIndex -and
          $finalStatusIndex -lt $initialRespondIndex)) {
  throw 'AddSession must respond only from a final STATUS taken after every inherited, persistent, app, schema, and inline option.'
}

$findSessionResulting = Get-ResultingHunkText -Patch $patchText -FunctionName 'RimeWithWeaselHandler::FindSession'
if ($findSessionResulting -match 'get_session_status\s*\(|m_session_status_map\s*\[' -or
    $findSessionResulting -notmatch 'm_session_status_map\.find\(ipc_id\)') {
  throw 'FindSession must use a read-only map lookup and never insert a phantom session.'
}

foreach ($functionName in @(
    'RimeWithWeaselHandler::RemoveSession',
    'RimeWithWeaselHandler::ProcessKeyEvent',
    'RimeWithWeaselHandler::CommitComposition',
    'RimeWithWeaselHandler::ClearComposition',
    'RimeWithWeaselHandler::SelectCandidateOnCurrentPage',
    'RimeWithWeaselHandler::HighlightCandidateOnCurrentPage',
    'RimeWithWeaselHandler::ChangePage',
    'RimeWithWeaselHandler::FocusIn',
    'RimeWithWeaselHandler::FocusOut',
    'RimeWithWeaselHandler::UpdateInputPosition',
    'RimeWithWeaselHandler::_Respond',
    'RimeWithWeaselHandler::_UpdateUI')) {
  $resulting = Get-ResultingHunkText -Patch $patchText -FunctionName $functionName
  if ($resulting -notmatch 'find_session_status\(ipc_id\)' -or
      $resulting -match 'm_session_status_map\s*\[') {
    throw "$functionName must fail/no-op on an unknown id without creating a map entry."
  }
}

$addedTryEmplace = [regex]::Matches(
  $patchText, '(?m)^\+.*m_session_status_map\.try_emplace\(ipc_id\)')
if ($addedTryEmplace.Count -ne 1) {
  throw 'Exactly one session-creation site is allowed: AddSession::try_emplace.'
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
$swapIndex = $selectSchemaResulting.IndexOf(
  'std::swap(replacement.session_status->engine_ctx')
$destroyOldIndex = if ($swapIndex -ge 0) {
  $selectSchemaResulting.IndexOf(
    '_DestroyEngineContext(replacement.context)', $swapIndex)
} else {
  -1
}
$disarmIndex = if ($swapIndex -ge 0) {
  $selectSchemaResulting.IndexOf('rollback.Disarm()', $swapIndex)
} else {
  -1
}
if ($prepareLoopIndex -lt 0 -or $stageIndex -lt 0 -or $commitLoopIndex -lt 0 -or
    $swapIndex -lt 0 -or $disarmIndex -lt 0 -or $destroyOldIndex -lt 0 -or
    -not ($prepareLoopIndex -lt $stageIndex -and
          $stageIndex -lt $commitLoopIndex -and
          $commitLoopIndex -lt $swapIndex -and
          $swapIndex -lt $disarmIndex -and
          $disarmIndex -lt $destroyOldIndex)) {
  throw 'SelectSchema must stage every validated replacement, atomically swap all sessions, disarm rollback, then destroy old contexts.'
}

$switchCreateFailure = [regex]::Match(
  $selectSchemaResulting,
  '(?ms)in_progress\s*=\s*new_ctx\s*;\s*' +
  'if\s*\(\s*create_context_result\s*!=\s*FAMO_ENGINE_OK\s*\|\|\s*!new_ctx\s*\)\s*\{.*?' +
  'return\s+false\s*;\s*\}')
$switchValidationFailure = [regex]::Match(
  $selectSchemaResulting,
  '(?ms)if\s*\(\s*!_RefillAbiView\(new_ctx\)\s*\)\s*\{.*?' +
  'return\s+false\s*;\s*\}')
if (-not $switchCreateFailure.Success -or
    -not $switchValidationFailure.Success -or
    $selectSchemaResulting.IndexOf($switchCreateFailure.Value) -gt
      $commitLoopIndex -or
    $selectSchemaResulting.IndexOf($switchValidationFailure.Value) -gt
      $commitLoopIndex) {
  throw 'SelectSchema must route creation and validation failures through the armed transaction guard before commit.'
}

$guardIndex = $selectSchemaResulting.IndexOf('class SchemaStagingGuard')
$guardDestructorIndex =
  $selectSchemaResulting.IndexOf('~SchemaStagingGuard() noexcept')
$createContextIndex =
  $selectSchemaResulting.IndexOf('m_engine_host.CreateContext(&schema')
$outerCatchIndex =
  $selectSchemaResulting.IndexOf('catch (const std::exception& error)')
if ($guardIndex -lt 0 -or $guardDestructorIndex -lt $guardIndex -or
    $createContextIndex -lt $guardDestructorIndex -or
    $outerCatchIndex -lt $createContextIndex) {
  throw 'SelectSchema must arm a no-unwind RAII rollback guard before creating contexts and catch staging exceptions at the handler boundary.'
}

foreach ($required in @(
    'std::vector<SessionStyleSnapshot> session_styles',
    'session_styles.reserve(m_session_status_map.size())',
    'session_styles.push_back({&pair.second, pair.second.style})',
    'auto prior_show_notifications = m_show_notifications',
    'UIStyle prior_ui_style = m_ui ? m_ui->style() : UIStyle()',
    'FamoEngineContext* in_progress = nullptr',
    '(void)abi_result_.Reset()',
    '_DestroyEngineContext(in_progress_)',
    '_DestroyEngineContext(replacement.context)',
    'std::swap(snapshot.session_status->style, snapshot.style)',
    'show_notifications_.swap(prior_show_notifications_)',
    'std::swap(ui_->style(), prior_ui_style_)',
    'in_progress = nullptr',
    'catch (...)')) {
  if (-not $selectSchemaResulting.Contains($required)) {
    throw "SelectSchema exception rollback is incomplete: $required"
  }
}
foreach ($snapshotMarker in @(
    'session_styles.push_back({&pair.second, pair.second.style})',
    'auto prior_show_notifications = m_show_notifications',
    'replacements.reserve(m_session_status_map.size())')) {
  if ($selectSchemaResulting.IndexOf($snapshotMarker) -gt
      $createContextIndex) {
    throw "SelectSchema must complete fallible snapshot/allocation before the first context is created: $snapshotMarker"
  }
}
foreach ($required in @(
    'm_engine_host.SetProperty(',
    '_ApplyFamoOptions(new_ctx)',
    '_ApplyAppOptions(new_ctx, pair.second.client_app)',
    'm_engine_host.GetOption(pair.second.engine_ctx, &ascii_name',
    '_SetInlinePreeditOptions(new_ctx, staged_style.inline_preedit)',
    'std::swap(replacement.session_status->style',
    'rollback.Disarm()',
    'schema commit requires a no-throw UIStyle swap')) {
  if (-not $selectSchemaResulting.Contains($required)) {
    throw "SelectSchema must preserve persistent/global/app/schema state before swapping contexts: $required"
  }
}
$propertyReplayIndex = $selectSchemaResulting.IndexOf(
  'm_engine_host.SetProperty(')
if ($propertyReplayIndex -lt 0 -or
    $propertyReplayIndex -gt
      $selectSchemaResulting.IndexOf('_ApplyFamoOptions(new_ctx)')) {
  throw 'SelectSchema must restore client_app before replaying options on a staged context.'
}
$selectFinalStatus = $selectSchemaResulting.LastIndexOf('_RefillAbiView(new_ctx)')
if ($selectFinalStatus -lt
    $selectSchemaResulting.IndexOf('_SetInlinePreeditOptions(new_ctx')) {
  throw 'SelectSchema must validate a final STATUS after all staged options are applied.'
}

foreach ($required in @(
    'struct ConfigCloseGuard',
    '~ConfigCloseGuard() noexcept',
    'rime_api->config_close(config)',
    'close_config{&config}')) {
  if (-not $patchText.Contains($required)) {
    throw "_LoadSchemaSpecificSettings must close an opened RimeConfig while unwinding: $required"
  }
}

foreach ($required in @(
    'm_engine_host.LoadV2',
    'm_engine_host.V2Runnable',
    'm_engine_host.ExecuteAction',
    'm_engine_host.ExecuteActionRecovering',
    'm_engine_host.SetProperty',
    'FamoEngineHost::ValidateResultV2',
    'FamoEngineActionResultLease',
    'FamoCompositionViewV2',
    'view.candidate_stride',
    '&RimeWithWeaselHandler::OnNotify',
    'const FamoUtf8String* state_label',
    'notification.message_label =',
    'm_notification_targets',
    'm_session_notifications',
    'm_unbound_notifications',
    'm_global_notifications',
    'engine_generation')) {
  if (-not $patchText.Contains($required)) {
    throw "engine-abi.patch must contain v2 contract token: $required"
  }
}

$executeAction = Get-ResultingHunkText -Patch $patchText -FunctionName 'RimeWithWeaselHandler::_ExecuteAbiAction'
foreach ($required in @(
    'pending_recovery_action',
    'pending_recovery_handled',
    'pending_response_result',
    'FAMO_ENGINE_ACTION_RECOVER',
    'recovery.value = static_cast<int32_t>(original_action)',
    'for (uint32_t attempt = 0; attempt < 3u; ++attempt)',
    'if (!recovered)',
    'The current physical input never entered the engine',
    'result.handled = false',
    'session_status->pending_response_result = std::move(m_abi_result)',
    'std::is_nothrow_move_assignable',
    'result.ready = true',
    'result.recovered_prior = true',
    'session_status->pending_recovery_action = 0',
    'm_engine_host.ExecuteActionRecovering(',
    'session_status->engine_ctx, &request, 3u',
    'recovery_outcome.recovery_pending',
    'result.handled = recovery_outcome.handled',
    'result_flags != 0',
    'kMaxViewCandidates = 4096u',
    'kMaxPendingResponseStringBytes = 1024u * 1024u')) {
  if (-not $executeAction.Contains($required)) {
    throw "Recovery-aware action dispatch is missing contract token: $required"
  }
}
$pendingResponseIndex = $executeAction.IndexOf(
  'if (session_status->pending_response_result)')
$pendingReadyIndex = $executeAction.IndexOf(
  'result.ready = true', $pendingResponseIndex)
$pendingRecoveredPriorIndex = $executeAction.IndexOf(
  'result.recovered_prior = true', $pendingReadyIndex)
$pendingResponseReturnIndex = $executeAction.IndexOf(
  'return result;', $pendingRecoveredPriorIndex)
$pendingBranchIndex = $executeAction.IndexOf(
  'if (session_status->pending_recovery_action != 0)')
$recoverDispatchIndex = $executeAction.IndexOf(
  'm_engine_host.ExecuteAction(session_status->engine_ctx, &recovery')
$failedRecoveryIndex = $executeAction.IndexOf('if (!recovered)')
$failOpenIndex = $executeAction.IndexOf(
  'result.handled = false', $failedRecoveryIndex)
$pendingReturnIndex = $executeAction.IndexOf(
  'return result;', $failedRecoveryIndex)
$adoptRecoveredResultIndex = $executeAction.IndexOf(
  'session_status->pending_response_result = std::move(m_abi_result)',
  $pendingReturnIndex)
$clearPendingIndex = $executeAction.IndexOf(
  'session_status->pending_recovery_action = 0', $adoptRecoveredResultIndex)
$recoveredReadyIndex = $executeAction.IndexOf(
  'result.ready = true', $clearPendingIndex)
$recoveredReturnIndex = $executeAction.IndexOf(
  'return result;', $recoveredReadyIndex)
$businessDispatchIndex = $executeAction.IndexOf(
  'm_engine_host.ExecuteActionRecovering(')
if ($pendingResponseIndex -lt 0 -or
    $pendingReadyIndex -lt $pendingResponseIndex -or
    $pendingRecoveredPriorIndex -lt $pendingReadyIndex -or
    $pendingResponseReturnIndex -lt $pendingRecoveredPriorIndex -or
    $pendingBranchIndex -lt $pendingResponseReturnIndex -or
    $recoverDispatchIndex -lt $pendingBranchIndex -or
    $failedRecoveryIndex -lt $recoverDispatchIndex -or
    $failOpenIndex -lt $failedRecoveryIndex -or
    $pendingReturnIndex -lt $failOpenIndex -or
    $adoptRecoveredResultIndex -lt $pendingReturnIndex -or
    $clearPendingIndex -lt $adoptRecoveredResultIndex -or
    $recoveredReadyIndex -lt $clearPendingIndex -or
    $recoveredReturnIndex -lt $recoveredReadyIndex -or
    $businessDispatchIndex -lt $recoveredReturnIndex) {
  throw ('Pending RECOVER must finish within the current callback: on failure ' +
         'the last physical input fails open before business dispatch; on ' +
         'success the exact final-result lease is retained and returned by this ' +
         'transaction without dispatching the current action.')
}
if ($executeAction.Contains('deferred_actions') -or
    $executeAction.Contains('pending_commit_prefix')) {
  throw ('A physical input must never be queued for a later callback: the last ' +
         'key has no guaranteed future event to drain such a queue, and a ' +
         'formatted prefix is not an exact engine-owned retry anchor.')
}
if ($executeAction -match
    'ValidateResultV2\([^)]*kMaxViewCandidates\s*=\s*64') {
  throw 'Ordinary action validation must not inherit the 64-candidate PEEK limit.'
}

$refillView = Get-ResultingHunkText -Patch $patchText -FunctionName 'RimeWithWeaselHandler::_RefillAbiView'
foreach ($required in @(
    'pending_recovery_action != 0',
    'pending_response_result',
    'ValidateResultV2(',
    'status.action, 4096u, 1024u * 1024u',
    'm_abi_result->result_flags != 0')) {
  if (-not $refillView.Contains($required)) {
    throw "STATUS validation must preserve the v2 recovery/cap contract: $required"
  }
}

$readClientInfo = Get-ResultingHunkText -Patch $patchText -FunctionName 'RimeWithWeaselHandler::_ReadClientInfo'
$setPropertyIndex = $readClientInfo.IndexOf(
  'm_engine_host.SetProperty(ctx, &name, &value)')
$applyAppIndex = $readClientInfo.IndexOf('_ApplyAppOptions(ctx, app_name)')
if ($setPropertyIndex -lt 0 -or $applyAppIndex -lt 0 -or
    $setPropertyIndex -gt $applyAppIndex) {
  throw '_ReadClientInfo must publish client_app through the v2 property seam before app options.'
}

$notifyHunk = [regex]::Matches(
  $patchText,
  '(?ms)^@@[^\r\n]*\r?\n(?<body>.*?)(?=^@@|^diff --git|\z)') |
  Where-Object {
    $_.Groups['body'].Value.Contains(
      'void FAMO_ENGINE_CALL RimeWithWeaselHandler::OnNotify')
  } |
  Select-Object -First 1
if ($null -eq $notifyHunk) {
  throw 'engine-abi.patch must contain the v2 notification callback hunk.'
}
$notifyResulting = (($notifyHunk.Groups['body'].Value -split '\r?\n') |
  Where-Object {
    ($_.StartsWith('+') -and -not $_.StartsWith('+++')) -or
    $_.StartsWith(' ')
  } |
  ForEach-Object { $_.Substring(1) }) -join "`n"
$notifyStart = $notifyResulting.IndexOf(
  'void FAMO_ENGINE_CALL RimeWithWeaselHandler::OnNotify')
$notifyEnd = $notifyResulting.IndexOf(
  'bool RimeWithWeaselHandler::_ReadClientInfo', $notifyStart)
if ($notifyStart -lt 0 -or $notifyEnd -lt $notifyStart) {
  throw 'Unable to isolate the complete OnNotify function for contract checks.'
}
$notifyFunction = $notifyResulting.Substring(
  $notifyStart, $notifyEnd - $notifyStart)
foreach ($required in @(
    'static_cast<RimeWithWeaselHandler*>(context_object)',
    'FamoString(*state_label)',
    'label.empty() ? notification.option_name : label',
    'handler->m_notification_targets.find(context)',
    'handler->m_session_notifications.find(key)',
    'handler->m_unbound_notifications.try_emplace(context)',
    'handler->m_global_notifications',
    'kMaxPendingNotifications',
    'kMaxUnboundContexts')) {
  if (-not $notifyFunction.Contains($required)) {
    throw "OnNotify must queue a bounded event for its exact opaque context: $required"
  }
}
if ($notifyFunction -match
      'get_state_label|rime_api->|m_session_status_map|m_engine_host') {
  throw 'OnNotify must consume the engine-resolved label without engine calls or unsynchronized session-map access.'
}

$bindResulting = Get-ResultingHunkText -Patch $patchText -FunctionName 'RimeWithWeaselHandler::_BindNotificationContext'
foreach ($required in @(
    'std::lock_guard<std::mutex> lock(m_notifier_mutex)',
    'EngineNotificationTarget{ipc_id, generation}',
    'm_session_notifications.try_emplace(key)',
    'queue.first->second.swap(unbound->second)',
    'm_unbound_notifications.erase(unbound)')) {
  if (-not $bindResulting.Contains($required)) {
    throw "Notification binding must atomically migrate CreateContext callbacks: $required"
  }
}

if ($patchText -notmatch
      '(?m)^\+\s*if\s*\(\s*!_TakeNotification\(engine_ctx,\s*engine_generation,\s*&notification\)' -or
    $patchText -match '(?m)^\+.*m_message_(?:type|value|label)') {
  throw '_ShowMessage must consume only the queue for its exact context generation.'
}

$selectBindIndex =
  $selectSchemaResulting.IndexOf('_BindNotificationContext(')
$selectBindingLockIndex =
  $selectSchemaResulting.IndexOf(
    'std::lock_guard<std::mutex> lock(m_notifier_mutex)',
    $selectBindIndex)
$selectOldUnbindIndex =
  $selectSchemaResulting.IndexOf(
    'm_notification_targets.erase(old_context)',
    $selectBindingLockIndex)
$selectGenerationSwapIndex =
  $selectSchemaResulting.IndexOf(
    'std::swap(replacement.session_status->engine_generation',
    $selectOldUnbindIndex)
if ($selectBindIndex -lt 0 -or $selectBindingLockIndex -lt 0 -or
    $selectOldUnbindIndex -lt 0 -or $selectGenerationSwapIndex -lt 0 -or
    -not ($selectBindIndex -lt $selectBindingLockIndex -and
          $selectBindingLockIndex -lt $selectOldUnbindIndex -and
          $selectOldUnbindIndex -lt $selectGenerationSwapIndex -and
          $selectGenerationSwapIndex -lt $disarmIndex)) {
  throw 'SelectSchema must atomically validate bindings, unbind the old context, and publish the new context generation before disarming rollback.'
}

$selectResulting = Get-ResultingHunkText -Patch $patchText -FunctionName 'RimeWithWeaselHandler::SelectCandidateOnCurrentPage'
$selectExecute = $selectResulting.IndexOf(
  '_ExecuteAbiAction(ipc_id, session_status, action, eat)')
$selectRespond = $selectResulting.IndexOf('_Respond(ipc_id, eat)')
if ($selectExecute -lt 0 -or $selectRespond -lt 0 -or
    $selectExecute -gt $selectRespond -or
    $selectResulting.Contains('_UpdateUI(ipc_id)')) {
  throw ('Selection must execute once and stage that same result; UI refresh ' +
         'belongs to the later application-ACK transition.')
}

$commitResulting = Get-ResultingHunkText -Patch $patchText -FunctionName 'RimeWithWeaselHandler::CommitComposition'
if ($commitResulting.IndexOf('FAMO_ENGINE_ACTION_COMMIT_COMPOSITION') -lt 0 -or
    $commitResulting.IndexOf('_Respond(ipc_id, eat)') -lt 0) {
  throw 'CommitComposition must return the same-action commit through EatLine.'
}

$serverSelect = Get-ResultingHunkText -Patch $patchText -FunctionName 'ServerImpl::OnSelectCandidateOnCurrentPage'
if ($serverSelect.IndexOf('return channel->TryWrite(msg)') -lt 0 -or
    $serverSelect.IndexOf('SelectCandidateOnCurrentPage(wParam, lParam,') -lt 0) {
  throw 'Selection IPC server must atomically preflight the EatLine response for the same result.'
}

foreach ($functionName in @(
    'RimeWithWeaselHandler::HighlightCandidateOnCurrentPage',
    'RimeWithWeaselHandler::ChangePage')) {
  $resulting = Get-ResultingHunkText -Patch $patchText -FunctionName $functionName
  if ($resulting -notmatch
      '(?s)const\s+bool\s+responded\s*=\s*action_outcome\.ready\s*&&\s*_Respond\(ipc_id,\s*eat\).*?return\s+responded\s*;' -or
      $resulting -match 'return\s+handled\s*&&\s*responded') {
    throw "$functionName must return response-ready independently of engine handled."
  }
}

$deliverStart = $patchText.IndexOf(
  '+bool RimeWithWeaselHandler::_DeliverPendingResponse(')
$deliverEnd = if ($deliverStart -ge 0) {
  $patchText.IndexOf(
    '+bool RimeWithWeaselHandler::_Respond(', $deliverStart)
} else {
  -1
}
if ($deliverStart -lt 0 -or $deliverEnd -le $deliverStart) {
  throw 'engine-abi.patch must contain the complete pending-response delivery function.'
}
$deliverSegment = $patchText.Substring(
  $deliverStart, $deliverEnd - $deliverStart)
$respondResulting = (($deliverSegment -split '\r?\n') |
  Where-Object { $_.StartsWith('+') -and -not $_.StartsWith('+++') } |
  ForEach-Object { $_.Substring(1) }) -join "`n"
foreach ($required in @(
    'UIStyle::PREVIEW_ALL',
    'for (uint32_t i = 0; i < view.candidate_count; ++i)',
    'i == view.highlighted_index ? mark_text',
    'session_status.style.label_font_point > 0',
    'session_status.style.comment_font_point > 0',
    'kResponseBufferBytes = 8u * 1024u * 1024u',
    'kResponseReserveBytes = 64u * 1024u',
    'if (body.size() > max_body_wchars)',
    'view.state_flags & FAMO_COMPOSITION_HAS_COMMIT',
    'escape_string(u8tow(commit_utf8))',
    'pending_response_result',
    'kMaxPendingCommitBytes = 1024u * 1024u',
    '_FailResponseFormattingOnceForTesting()',
    'throw std::bad_alloc()',
    'if (!eat(response))',
    'pending_response_generation',
    'last_acknowledged_response_generation',
    'm_next_response_delivery_generation++',
    'famo.delivery_session=',
    'famo.delivery_generation=',
    'm_staged_response_delivery =',
    'catch (...)',
    'engine-owned lease remains the exact retry anchor',
    'Commit itself violates the bounded ABI/pipe contract')) {
  if (-not $respondResulting.Contains($required)) {
    throw "_DeliverPendingResponse must preserve exact, bounded, no-unwind delivery: $required"
  }
}
if ($respondResulting.Contains('pending_response_result.Reset()')) {
  throw 'Formatting or staging a response must not release its engine-owned lease before application ACK.'
}
if ([regex]::Matches($respondResulting, '\beat\s*\(').Count -ne 1 -or
    $respondResulting.Contains('eat(header)') -or
    $respondResulting.Contains('eat(body)')) {
  throw 'Each response document must reach EatLine in one atomic callback.'
}
$fullCommitIndex = $respondResulting.IndexOf(
  'escape_string(u8tow(commit_utf8))')
$overflowIndex = $respondResulting.IndexOf(
  'if (body.size() > max_body_wchars)')
$fallbackCommitIndex = $respondResulting.IndexOf(
  'escape_string(u8tow(commit_utf8))', $overflowIndex)
$terminalIndex = $respondResulting.IndexOf(
  'if (body.size() > max_body_wchars)', $overflowIndex + 1)
if ($overflowIndex -lt 0 -or $fallbackCommitIndex -lt $overflowIndex -or
    $terminalIndex -lt $fallbackCommitIndex -or
    $fullCommitIndex -eq $fallbackCommitIndex) {
  throw 'Response overflow must rebuild a commit-preserving minimal body before its finite terminal check.'
}

$ackDelivery = Get-ResultingHunkText -Patch $patchText -FunctionName 'RimeWithWeaselHandler::AcknowledgeResponseDelivery'
foreach ($required in @(
    'last_acknowledged_response_generation',
    'pending_response_generation',
    'token.generation',
    'pending_response_result',
    'provisional_session = false',
    'pending_response_generation = 0',
    'session_status->__synced = true',
    'pending_response_result.Reset()',
    '_UpdateUI(token.session_id)',
    'catch (...)')) {
  if (-not $ackDelivery.Contains($required)) {
    throw "Application ACK must be exact, idempotent, and no-unwind before releasing the lease: $required"
  }
}
$legacyConfirm = Get-ResultingHunkText -Patch $patchText -FunctionName 'RimeWithWeaselHandler::ConfirmResponsePipeWrite'
if ($legacyConfirm -notmatch
      '(?s)if\s*\(\s*!token\s*\|\|\s*token\.requires_client_ack\s*\)\s*return\s*;.*?' +
      'AcknowledgeResponseDelivery\(token\)') {
  throw 'Only legacy clients may release a delivery immediately after the accepted pipe write.'
}

$readClientInfoDelivery = Get-ResultingHunkText -Patch $patchText -FunctionName 'RimeWithWeaselHandler::_ReadClientInfo'
foreach ($required in @(
    'session.delivery_ack_v1=1',
    'delivery_ack_v1 = true',
    'provisional_session =',
    'client_creation_nonce')) {
  if (-not $readClientInfoDelivery.Contains($required)) {
    throw "START must negotiate ACK capability and bind a recoverable creation nonce: $required"
  }
}
$addSessionDelivery = Get-ResultingHunkText -Patch $patchText -FunctionName 'RimeWithWeaselHandler::AddSession'
foreach ($required in @(
    'RimeWithWeaselHandler::AddSessionForClient(',
    'FamoQueryProcessCreationTime(client_process_id)',
    'for (auto it = m_session_status_map.begin()',
    'now - status.provisional_created_at > 5u * 60u * 1000u',
    '(void)RemoveSession(current->first)',
    'for (auto probe = m_session_status_map.begin()',
    'checked_earlier',
    'FamoObservedClientIsAlive(observed_pid, observed_created)',
    'current->second.client_process_id == observed_pid',
    'current->second.client_process_created == observed_created',
    'constexpr size_t kMaximumObservedSessions = 4096',
    'm_session_status_map.size() >= kMaximumObservedSessions',
    '_ReadSessionCreationNonce(buffer)',
    'provisional_session',
    'provisional_created_at',
    '5u * 60u * 1000u',
    'status.client_creation_nonce == creation_nonce',
    '_DeliverPendingResponse(pair.first, &status, eat)',
    'session_status.client_process_id = client_process_id',
    'session_status.client_process_created = client_process_created')) {
  if (-not $addSessionDelivery.Contains($required)) {
    throw "START recovery must bound and reclaim observed client sessions: $required"
  }
}
$provisionalSweepIndex = $addSessionDelivery.IndexOf(
  'for (auto it = m_session_status_map.begin()')
$orphanSweepIndex = $addSessionDelivery.IndexOf(
  'for (auto probe = m_session_status_map.begin()', $provisionalSweepIndex)
$nonceRecoveryIndex = $addSessionDelivery.IndexOf(
  'const DWORD creation_nonce = _ReadSessionCreationNonce(buffer)',
  $orphanSweepIndex)
$sessionCapIndex = $addSessionDelivery.IndexOf(
  'constexpr size_t kMaximumObservedSessions = 4096',
  $orphanSweepIndex)
$createForNewSessionIndex = $addSessionDelivery.IndexOf(
  'm_engine_host.CreateContext(', $nonceRecoveryIndex)
if ($provisionalSweepIndex -lt 0 -or $orphanSweepIndex -lt 0 -or
    $nonceRecoveryIndex -lt 0 -or $sessionCapIndex -lt 0 -or
    $createForNewSessionIndex -lt 0 -or
    -not ($provisionalSweepIndex -lt $orphanSweepIndex -and
          $orphanSweepIndex -lt $nonceRecoveryIndex -and
          $nonceRecoveryIndex -lt $sessionCapIndex -and
          $sessionCapIndex -lt $createForNewSessionIndex)) {
  throw ('START must expire provisional/dead clients, allow an exact nonce ' +
         'retry even at the session cap, then enforce the cap only before ' +
         'allocating a genuinely new engine context.')
}

$processObservation = Get-ResultingHunkText -Patch $patchText -FunctionName 'FamoQueryProcessCreationTime'
if ($processObservation -notmatch
      '(?s)FamoQueryProcessCreationTime\(DWORD\s+process_id\).*?' +
      'OpenProcess\(.*?GetProcessTimes\(process.*?' +
      '::CloseHandle\(process\).*?return\s+queried' -or
    $processObservation -notmatch
      '(?s)FamoObservedClientIsAlive\(DWORD\s+process_id,.*?' +
      'OpenProcess\(.*?WaitForSingleObject\(process,\s*0\).*?' +
      'WAIT_OBJECT_0.*?::CloseHandle\(process\).*?return\s+false;.*?' +
      'GetProcessTimes\(process.*?::CloseHandle\(process\)') {
  throw 'Every observed-process liveness/creation-time path must close its kernel handle.'
}

$clientConsume = Get-ResultingHunkText -Patch $patchText -FunctionName 'ClientImpl::GetCommitResponseData'
foreach ($required in @(
    'pending_response_body_length == 0 || pending_response_claimed',
    'pending_response_claimed = true',
    'parsed = channel.HandleResponseData(handler)',
    'catch (...)',
    'if (!parsed)',
    'pending_response_claimed = false')) {
  if (-not $clientConsume.Contains($required)) {
    throw "A response envelope must have one claimed consumer, with retry only after parse failure: $required"
  }
}
$claimIndex = $clientConsume.IndexOf('pending_response_claimed = true')
$parseIndex = $clientConsume.IndexOf(
  'parsed = channel.HandleResponseData(handler)')
if ($claimIndex -lt 0 -or $parseIndex -lt 0 -or $claimIndex -gt $parseIndex) {
  throw 'The envelope claim must be published before invoking a reentrant/allocating parser.'
}
if ($clientConsume -match
      '\bpending_response_body\b|std::vector\s*<\s*wchar_t\s*>|std::copy\s*\(') {
  throw 'Response consumption must parse PipeChannel storage in place without a second owned body.'
}

$clientRelease = Get-ResultingHunkText -Patch $patchText -FunctionName 'ClientImpl::ReleaseResponseDataClaim'
foreach ($required in @(
    'pending_response_claimed',
    '!pending_response_applied',
    'pending_response_delivery.session_id == token.session_id',
    'pending_response_delivery.generation == token.generation',
    'pending_response_delivery.requires_client_ack',
    'pending_response_claimed = false')) {
  if (-not $clientRelease.Contains($required)) {
    throw "Only the exact, unapplied consumer may release its envelope claim: $required"
  }
}

$clientSend = Get-ResultingHunkText -Patch $patchText -FunctionName 'ClientImpl::_SendMessage'
foreach ($required in @(
    'TransactionDeliveryState::RequestNotDispatched',
    'if (!channel.PrepareTransactionBuffer())',
    'channel.ClearTransactionBuffer()',
    'bool request_sent = false',
    'pending_response_body_length != 0',
    'const LRESULT result = channel.Transact(req, &request_sent)',
    'const auto inspect_response',
    'channel.HandleResponseData(inspect_response)',
    'received_body_length = used + 1u',
    'pending_response_body_length = received_body_length',
    'next_local_response_generation++',
    'local_session, local_generation, false',
    'pending_response_claimed = false',
    'TransactionDeliveryState::Completed',
    'if (request_sent)',
    'TransactionDeliveryState::ResponseUncertain',
    'catch (...)',
    'channel.DisconnectNoexcept()')) {
  if (-not $clientSend.Contains($required)) {
    throw "Direct-buffer response ownership/uncertainty is missing a contract token: $required"
  }
}
$prepareIndex = $clientSend.IndexOf(
  'if (!channel.PrepareTransactionBuffer())')
$transactIndex = $clientSend.IndexOf(
  'const LRESULT result = channel.Transact(req, &request_sent)')
$inspectIndex = $clientSend.IndexOf(
  'channel.HandleResponseData(inspect_response)')
$publishLengthIndex = $clientSend.IndexOf(
  'pending_response_body_length = received_body_length')
$completedIndex = $clientSend.IndexOf(
  'TransactionDeliveryState::Completed', $transactIndex)
$uncertainIndex = $clientSend.IndexOf(
  'TransactionDeliveryState::ResponseUncertain', $completedIndex)
if ($prepareIndex -lt 0 -or $transactIndex -lt 0 -or $inspectIndex -lt 0 -or
    $publishLengthIndex -lt 0 -or $completedIndex -lt 0 -or
    $uncertainIndex -lt 0 -or
    -not ($prepareIndex -lt $transactIndex -and
          $transactIndex -lt $inspectIndex -and
          $inspectIndex -lt $publishLengthIndex -and
          $publishLengthIndex -lt $completedIndex -and
          $completedIndex -lt $uncertainIndex)) {
  throw ('PipeChannel storage must be allocated before dispatch, inspected ' +
         'in place, published only after validation, and classified uncertain ' +
         'only when a request was actually sent.')
}
if ($clientSend -match
      '\bpending_response_body\b|std::vector\s*<\s*wchar_t\s*>|' +
      'std::copy\s*\(|\.reserve\s*\(\s*kFamoOwnedResponseWchars') {
  throw ('The client must retain PipeChannel''s 8 MiB receive buffer directly; ' +
         'a second vector/body copy would reintroduce post-read allocation.')
}

foreach ($required in @(
    'enum class TransactionDeliveryState : uint8_t',
    'Completed = 0',
    'RequestNotDispatched = 1',
    'ResponseUncertain = 2',
    'TransactionDeliveryState LastTransactionDeliveryState() const noexcept')) {
  if (-not $patchText.Contains($required)) {
    throw "The public client contract must expose request-delivery uncertainty: $required"
  }
}

$readDelivery = Get-ResultingHunkText -Patch $patchText -FunctionName 'FamoReadResponseDelivery'
if ($readDelivery.IndexOf('token->requires_client_ack = true') -lt 0) {
  throw 'Only an explicit remote delivery marker may create a network-ACK token.'
}

$clientAck = Get-ResultingHunkText -Patch $patchText -FunctionName 'ClientImpl::_AcknowledgePendingResponse'
foreach ($required in @(
    '!pending_response_delivery.requires_client_ack',
    'PipeMessage ack{WEASEL_IPC_ACK_DELIVERY',
    'if (ack_result != 1)',
    'session_id = 0',
    'session_start_pending = false',
    'channel.DisconnectNoexcept()',
    'catch (...)')) {
  if (-not $clientAck.Contains($required)) {
    throw "ACK must retire legacy envelopes locally and fail stale incarnations closed/no-unwind: $required"
  }
}
$localRetireIndex = $clientAck.IndexOf(
  '!pending_response_delivery.requires_client_ack')
$remoteAckIndex = $clientAck.IndexOf(
  'PipeMessage ack{WEASEL_IPC_ACK_DELIVERY')
if ($localRetireIndex -lt 0 -or $remoteAckIndex -lt 0 -or
    $localRetireIndex -gt $remoteAckIndex) {
  throw 'A new client must retire an old-server envelope locally before any unsupported ACK transaction.'
}

$clientEnd = Get-ResultingHunkText -Patch $patchText -FunctionName 'ClientImpl::EndSession'
foreach ($required in @(
    'void ClientImpl::EndSession() noexcept',
    'channel.Transact(end)',
    'catch (...)',
    'channel.DisconnectNoexcept()',
    'session_id = 0')) {
  if (-not $clientEnd.Contains($required)) {
    throw "Logical teardown must bypass the business gate without throwing across destruction/COM: $required"
  }
}
if (-not $patchText.Contains('ClientImpl::~ClientImpl() noexcept') -or
    -not $patchText.Contains('void ClientImpl::Disconnect() noexcept') -or
    -not $patchText.Contains('void DisconnectNoexcept() noexcept')) {
  throw 'Client destruction and physical disconnect must be allocation-safe and no-unwind.'
}

$serverDispatch = Get-ResultingHunkText -Patch $patchText -FunctionName 'ServerImpl::HandlePipeMessage'
foreach ($required in @(
    'TakeStagedResponseDelivery()',
    'WEASEL_IPC_ACK_DELIVERY',
    'pipe_msg.Msg == WEASEL_IPC_START_SESSION ? result : pipe_msg.lParam',
    'm_pRequestHandler->RemoveSession(result)',
    'resp(result)',
    'ConfirmResponsePipeWrite(token)')) {
  if (-not $serverDispatch.Contains($required)) {
    throw "Server delivery must bind the accepted pipe write to its exact START/session token: $required"
  }
}
$acceptedWriteIndex = $serverDispatch.IndexOf('resp(result)')
$confirmWriteIndex = $serverDispatch.IndexOf(
  'ConfirmResponsePipeWrite(token)')
if ($acceptedWriteIndex -lt 0 -or $confirmWriteIndex -lt 0 -or
    $acceptedWriteIndex -gt $confirmWriteIndex) {
  throw 'A delivery lease may transition only after the accepted pipe write succeeds.'
}

$serverPipeThread = Get-ResultingHunkText -Patch $patchText -FunctionName 'PipeServer::_ProcessPipeThread'
foreach ($required in @(
    'ULONG client_process_id = 0',
    'GetNamedPipeClientProcessId(pipe, &client_process_id)',
    'msg, static_cast<DWORD>(client_process_id)',
    '_SendAcceptedResponse(pipe, resp)')) {
  if (-not $serverPipeThread.Contains($required)) {
    throw "The accepted pipe must bind START and response delivery to its exact client: $required"
  }
}

$serverStart = Get-ResultingHunkText -Patch $patchText -FunctionName 'ServerImpl::OnStartSession'
if ($serverStart -notmatch
      '(?s)AddSessionForClient\(.*?channel->ReceiveBuffer\(\).*?' +
      'channel->TryWrite\(msg\).*?,\s*lParam\s*\)') {
  throw 'START must pass the accepted pipe client PID into AddSessionForClient.'
}

$acceptedSend = Get-ResultingHunkText -Patch $patchText -FunctionName '_SendAccepted(HANDLE pipe'
if ($acceptedSend.IndexOf('_WritePipe(pipe, data_sz, pbuff)') -lt 0 -or
    $acceptedSend.IndexOf('ClearTransactionBuffer()') -lt 0 -or
    $acceptedSend.Contains('_Reconnect()')) {
  throw 'A server response must use its exact accepted handle and never reconnect to acknowledge a false peer.'
}

$claimGuard = Get-ResultingHunkText -Patch $patchText -FunctionName 'class ResponseClaimGuard'
foreach ($required in @(
    '~ResponseClaimGuard() noexcept',
    'if (!claimed_ || !token_)',
    'if (applied_)',
    'client_.MarkResponseDataApplied(token_)',
    'client_.ReleaseResponseDataClaim(token_)',
    'catch (...)')) {
  if (-not $claimGuard.Contains($required)) {
    throw "The edit-session claim guard must settle the exact response without unwinding: $required"
  }
}

$responseApplySession = Get-ResultingHunkText -Patch $patchText -FunctionName 'class CResponseApplyEditSession'
foreach ($required in @(
    'com_ptr<ITfContext> context',
    'const weasel::ResponseDeliveryToken& delivery',
    'CEditSession(text_service, context)',
    'delivery_(delivery)',
    '_ApplyResponseInEditSession(',
    '_pContext, ec, delivery_')) {
  if (-not $responseApplySession.Contains($required)) {
    throw "The response edit session must carry one exact context/token pair: $required"
  }
}

$editApply = Get-ResultingFileText `
  -Patch $patchText `
  -FilePath 'WeaselTSF/EditSession.cpp'
$editApplyStart = $editApply.IndexOf(
  'HRESULT WeaselTSF::_ApplyResponseInEditSession')
$editApplyEnd = $editApply.IndexOf(
  '/* 法墨标点配对：commit 后注入方向键移动光标', $editApplyStart)
if ($editApplyStart -lt 0 -or $editApplyEnd -le $editApplyStart) {
  throw 'Unable to isolate the complete exact-context response apply function.'
}
$editApply = $editApply.Substring(
  $editApplyStart, $editApplyEnd - $editApplyStart)
foreach ($required in @(
    'const weasel::ResponseDeliveryToken& expected_delivery',
    'pending_delivery.session_id != expected_delivery.session_id',
    'pending_delivery.generation != expected_delivery.generation',
    'pending_delivery.requires_client_ack !=',
    'expected_delivery.requires_client_ack',
    'm_client.GetCommitResponseData(std::ref(parser))',
    '_FamoProbeNextChar(response_context, ec',
    '_StartCompositionInEditSession(',
    'response_context, ec, config.inline_preedit',
    '_InsertTextInEditSession(',
    'response_context, ec, commit, &durable',
    'response_applied = durable',
    '_EndCompositionInEditSession(response_context, ec, false)',
    '_ShowInlinePreeditInEditSession(',
    'response_context, ec, context',
    '_CompleteResponseContext(response_delivery)',
    'catch (...)')) {
  if (-not $editApply.Contains($required)) {
    throw "The exact READWRITE response transaction is missing a contract token: $required"
  }
}
foreach ($forbiddenApply in @(
    'RequestEditSession\s*\(',
    '\bCInsertTextEditSession\b',
    '\b_InsertText\s*\(',
    '\bAcknowledgeResponseData\s*\(',
    '\b_pEditSessionContext\b')) {
  if ($editApply -match $forbiddenApply) {
    throw ('Response parsing, SetText, and application marking must stay ' +
           'inside the granted exact-context READWRITE edit session; ' +
           "forbidden pattern: $forbiddenApply")
  }
}

$compositionFile = Get-ResultingFileText `
  -Patch $patchText `
  -FilePath 'WeaselTSF/Composition.cpp'
$insertApplyStart = $compositionFile.IndexOf(
  'HRESULT WeaselTSF::_InsertTextInEditSession')
$insertApplyEnd = $compositionFile.IndexOf(
  '/* Update Composition */', $insertApplyStart)
if ($insertApplyStart -lt 0 -or $insertApplyEnd -le $insertApplyStart) {
  throw 'Unable to isolate the synchronous SetText response barrier.'
}
$insertApply = $compositionFile.Substring(
  $insertApplyStart, $insertApplyEnd - $insertApplyStart)
foreach ($required in @(
    'if (durable)',
    '*durable = false',
    'if (!pContext || !durable || !_pComposition)',
    'pRange->SetText(',
    '*durable = true',
    'pRange->Collapse(ec, TF_ANCHOR_END)',
    'return pContext->SetSelection(ec, 1, &selection)')) {
  if (-not $insertApply.Contains($required)) {
    throw "SetText must be the exact durable response barrier: $required"
  }
}
$setTextIndex = $insertApply.IndexOf('pRange->SetText(')
$durableIndex = $insertApply.IndexOf('*durable = true', $setTextIndex)
$collapseIndex = $insertApply.IndexOf(
  'pRange->Collapse(ec, TF_ANCHOR_END)', $durableIndex)
$selectionIndex = $insertApply.IndexOf(
  'pContext->SetSelection(ec, 1, &selection)', $collapseIndex)
if ($setTextIndex -lt 0 -or $durableIndex -lt 0 -or
    $collapseIndex -lt 0 -or $selectionIndex -lt 0 -or
    -not ($setTextIndex -lt $durableIndex -and
          $durableIndex -lt $collapseIndex -and
          $collapseIndex -lt $selectionIndex) -or
    $insertApply.Contains('RequestEditSession(')) {
  throw ('SetText success must mark the commit durable before fallible ' +
         'selection work, without spawning another edit session.')
}

$prepareResponseContext = Get-ResultingHunkText -Patch $patchText -FunctionName 'WeaselTSF::_PrepareResponseContext'
foreach ($required in @(
    '_responseOriginContext && !_responseOriginDelivery',
    '!m_client.PendingResponseDelivery()',
    'm_client.LastTransactionDeliveryState() !=',
    'weasel::TransactionDeliveryState::ResponseUncertain',
    '_responseOriginContext.Release()',
    'if (!context || _responseOriginContext)',
    '_responseOriginContext = context',
    'catch (...)')) {
  if (-not $prepareResponseContext.Contains($required)) {
    throw "Origin context capture must survive an uncertain response read: $required"
  }
}

$completeResponseContext = Get-ResultingHunkText -Patch $patchText -FunctionName 'WeaselTSF::_CompleteResponseContext'
foreach ($required in @(
    'SameResponseDelivery(_responseOriginDelivery, token)',
    '_responseOriginDelivery = {}',
    '_responseOriginContext.Release()')) {
  if (-not $completeResponseContext.Contains($required)) {
    throw "Only the exact applied delivery may release its origin context: $required"
  }
}

$updateComposition = Get-ResultingHunkText -Patch $patchText -FunctionName 'WeaselTSF::_UpdateComposition'
foreach ($required in @(
    'const weasel::ResponseDeliveryToken delivery =',
    'm_client.PendingResponseDelivery()',
    '_PrepareResponseContext(pContext)',
    'if (!_responseOriginDelivery)',
    '_responseOriginDelivery = delivery',
    'SameResponseDelivery(_responseOriginDelivery, delivery)',
    'new CResponseApplyEditSession(',
    'this, _responseOriginContext, delivery',
    '_responseOriginContext->RequestEditSession(',
    'TF_ES_ASYNCDONTCARE | TF_ES_READWRITE')) {
  if (-not $updateComposition.Contains($required)) {
    throw "Composition scheduling must stay bound to the initiating ITfContext/token: $required"
  }
}
if ($updateComposition.Contains('pContext->RequestEditSession(')) {
  throw 'A recovered response must never be retargeted to the currently focused context.'
}

$processKey = Get-ResultingHunkText -Patch $patchText -FunctionName 'WeaselTSF::_ProcessKeyEvent'
$bindOriginIndex = $processKey.IndexOf('_PrepareResponseContext(context)')
$sendKeyIndex = $processKey.IndexOf('m_client.ProcessKeyEvent(ke)')
if ($bindOriginIndex -lt 0 -or $sendKeyIndex -lt 0 -or
    $bindOriginIndex -gt $sendKeyIndex) {
  throw 'The initiating ITfContext must be retained before a key request can become response-uncertain.'
}

$clientImplFile = Get-ResultingFileText `
  -Patch $patchText `
  -FilePath 'WeaselIPC/WeaselClientImpl.cpp'
$closeTransportStart = $clientImplFile.IndexOf(
  'void ClientImpl::CloseTransport() noexcept')
$closeTransportEnd = $clientImplFile.IndexOf(
  'void ClientImpl::ShutdownServer()', $closeTransportStart)
if ($closeTransportStart -lt 0 -or
    $closeTransportEnd -le $closeTransportStart) {
  throw 'Unable to isolate ClientImpl::CloseTransport.'
}
$closeTransport = $clientImplFile.Substring(
  $closeTransportStart, $closeTransportEnd - $closeTransportStart)
if ($closeTransport -notmatch
      '(?s)^void\s+ClientImpl::CloseTransport\(\)\s+noexcept\s*\{\s*' +
      'channel\.DisconnectNoexcept\(\);\s*\}\s*$') {
  throw 'CloseTransport must close only the physical pipe and preserve the logical session/envelope.'
}

$reconnectHunk = Get-ResultingHunkText -Patch $patchText -FunctionName 'WeaselTSF::_Reconnect'
$reconnectStart = $reconnectHunk.IndexOf('bool WeaselTSF::_Reconnect() noexcept')
$reconnectEnd = $reconnectHunk.IndexOf(
  'static unsigned int retry', $reconnectStart)
if ($reconnectStart -lt 0 -or $reconnectEnd -le $reconnectStart) {
  throw 'Unable to isolate the complete transport-only reconnect function.'
}
$reconnect = $reconnectHunk.Substring(
  $reconnectStart, $reconnectEnd - $reconnectStart)
foreach ($required in @(
    'm_client.CloseTransport()',
    'if (!m_client.Connect(NULL))',
    'if (m_client.Echo())',
    '_SchedulePendingCommitResponse()',
    'm_client.PendingResponseDelivery()',
    'm_client.GetResponseData(std::ref(recovered_parser))',
    'm_client.AcknowledgeResponseData()',
    'm_client.StartSession()',
    'catch (...)')) {
  if (-not $reconnect.Contains($required)) {
    throw "Reconnect must recover the existing logical session before START: $required"
  }
}
$closeTransportIndex = $reconnect.IndexOf('m_client.CloseTransport()')
$connectIndex = $reconnect.IndexOf('m_client.Connect(NULL)')
$recoverEchoIndex = $reconnect.IndexOf('if (m_client.Echo())')
$newStartIndex = $reconnect.IndexOf('m_client.StartSession()')
if ($closeTransportIndex -lt 0 -or $connectIndex -lt 0 -or
    $recoverEchoIndex -lt 0 -or $newStartIndex -lt 0 -or
    -not ($closeTransportIndex -lt $connectIndex -and
          $connectIndex -lt $recoverEchoIndex -and
          $recoverEchoIndex -lt $newStartIndex) -or
    $reconnect -match '\bm_client\.(?:Disconnect|EndSession)\s*\(') {
  throw ('Reconnect must retain the logical session, Echo it first, and use ' +
         'START only after the server proves the old session is absent.')
}

$serverEcho = Get-ResultingHunkText -Patch $patchText -FunctionName 'ServerImpl::OnEcho'
$findEchoIndex = $serverEcho.IndexOf(
  'm_pRequestHandler->FindSession(lParam)')
$recoverEchoIndex = $serverEcho.IndexOf(
  'm_pRequestHandler->RecoverResponseDelivery(')
if ($findEchoIndex -lt 0 -or $recoverEchoIndex -lt 0 -or
    $findEchoIndex -gt $recoverEchoIndex -or
    -not $serverEcho.Contains('channel->TryWrite(msg)')) {
  throw 'ECHO must replay the retained exact response for an existing logical session.'
}
$recoverDelivery = Get-ResultingHunkText -Patch $patchText -FunctionName 'RimeWithWeaselHandler::RecoverResponseDelivery'
if ($recoverDelivery -notmatch
      '(?s)find_session_status\(ipc_id\).*?' +
      'pending_response_result.*?_DeliverPendingResponse\(ipc_id,\s*session_status,\s*eat\)') {
  throw 'Server ECHO recovery must stage only the retained result owned by that exact session.'
}

$startSessionBegin = $clientImplFile.IndexOf('void ClientImpl::StartSession()')
$startSessionEnd = $clientImplFile.IndexOf(
  'void ClientImpl::EndSession()', $startSessionBegin)
if ($startSessionBegin -lt 0 -or $startSessionEnd -le $startSessionBegin) {
  throw 'Unable to isolate ClientImpl::StartSession.'
}
$startSession = $clientImplFile.Substring(
  $startSessionBegin, $startSessionEnd - $startSessionBegin)
foreach ($required in @(
    'if (pending_response_delivery)',
    'if (!pending_response_applied ||',
    '!_AcknowledgePendingResponse()',
    'if (pending_response_body_length != 0)',
    'if (!session_start_pending)',
    '_RenewSessionCreationNonce()',
    'session_start_pending = true',
    'if (!_WriteClientInfo())',
    '_SendMessage(WEASEL_IPC_START_SESSION, 0, 0)',
    'channel.DisconnectNoexcept()',
    'catch (...)')) {
  if (-not $startSession.Contains($required)) {
    throw "START must gate the shared transaction buffer and retain its nonce across uncertainty: $required"
  }
}
$pendingStartIndex = $startSession.IndexOf('if (pending_response_delivery)')
$bodyStartIndex = $startSession.IndexOf(
  'if (pending_response_body_length != 0)', $pendingStartIndex)
$nonceStartIndex = $startSession.IndexOf(
  'if (!session_start_pending)', $bodyStartIndex)
$writeInfoIndex = $startSession.IndexOf(
  'if (!_WriteClientInfo())', $nonceStartIndex)
$sendStartIndex = $startSession.IndexOf(
  '_SendMessage(WEASEL_IPC_START_SESSION, 0, 0)', $writeInfoIndex)
if ($pendingStartIndex -lt 0 -or $bodyStartIndex -lt 0 -or
    $nonceStartIndex -lt 0 -or $writeInfoIndex -lt 0 -or
    $sendStartIndex -lt 0 -or
    -not ($pendingStartIndex -lt $bodyStartIndex -and
          $bodyStartIndex -lt $nonceStartIndex -and
          $nonceStartIndex -lt $writeInfoIndex -and
          $writeInfoIndex -lt $sendStartIndex) -or
    $startSession.Contains('session_start_pending = false')) {
  throw ('START must settle any previous envelope before touching client-info, ' +
         'then reuse the same nonce until a definite session result.')
}

$writeClientInfo = Get-ResultingHunkText -Patch $patchText -FunctionName 'ClientImpl::_WriteClientInfo'
$writeGateIndex = $writeClientInfo.IndexOf(
  'if (pending_response_delivery || pending_response_body_length != 0)')
$firstClientInfoWrite = $writeClientInfo.IndexOf('channel <<')
if ($writeGateIndex -lt 0 -or $firstClientInfoWrite -lt 0 -or
    $writeGateIndex -gt $firstClientInfoWrite -or
    -not $writeClientInfo.Contains('session.delivery_ack_v1=1') -or
    -not $writeClientInfo.Contains('session.creation_nonce=')) {
  throw 'Client-info must refuse to overwrite a retained response before writing its ACK/nonce negotiation.'
}

$weaselTsfFile = Get-ResultingFileText `
  -Patch $patchText `
  -FilePath 'WeaselTSF/WeaselTSF.cpp'
$ensureServerStart = $weaselTsfFile.IndexOf(
  'bool WeaselTSF::_EnsureServerConnected() noexcept')
if ($ensureServerStart -lt 0) {
  throw 'Unable to isolate WeaselTSF::_EnsureServerConnected.'
}
$ensureServer = $weaselTsfFile.Substring($ensureServerStart)
$pendingIndex = $ensureServer.IndexOf('m_client.PendingResponseDelivery()')
$echoIndex = $ensureServer.IndexOf('if (m_client.Echo())')
$reconnectIndex = $ensureServer.IndexOf('(void)_Reconnect()', $echoIndex)
if ($pendingIndex -lt 0 -or $echoIndex -lt 0 -or $reconnectIndex -lt 0 -or
    -not ($pendingIndex -lt $echoIndex -and $echoIndex -lt $reconnectIndex) -or
    -not $ensureServer.Contains('if (!m_client.ResponseDataApplied())') -or
    -not $ensureServer.Contains('return m_client.AcknowledgeResponseData()')) {
  throw 'Owned application state must be settled before Echo or transport reconnect.'
}

$testKeyDown = Get-ResultingHunkText -Patch $patchText -FunctionName 'WeaselTSF::OnTestKeyDown'
$keyDown = Get-ResultingHunkText -Patch $patchText -FunctionName 'WeaselTSF::OnKeyDown'
foreach ($required in @(
    '_fFamoTestKeyDeliveryPending',
    'm_client.PendingResponseDelivery()')) {
  if (-not $testKeyDown.Contains($required)) {
    throw "TestKeyDown must remember an unhandled response-bearing physical input: $required"
  }
}
if (-not $keyDown.Contains('_fFamoTestKeyDeliveryPending') -or
    -not $keyDown.Contains('*pfEaten = FALSE')) {
  throw 'The matching OnKeyDown must not redispatch a recovered-prior physical input while its child edit is delayed.'
}

$candidateView = Get-ResultingHunkText -Patch $patchText -FunctionName 'RimeWithWeaselHandler::_GetCandidateInfoFromView'
if ($candidateView -notmatch
      '(?s)const\s+uint32_t\s+n\s*=\s*view\.candidate_count\s*;.*?' +
      'for\s*\(\s*uint32_t\s+i\s*=\s*0\s*;\s*i\s*<\s*n\s*;\s*\+\+i\s*\)' -or
    $candidateView -match 'min\s*\([^)]*64|candidate_count\s*>\s*64') {
  throw 'Ordinary view rendering must consume the validated candidate_count, including counts above 64.'
}

$pipeHeader = Get-ResultingHunkText -Patch $patchText -FunctionName 'class PipeChannel'
if ($pipeHeader -notmatch
    'size_t\s+bs\s*=\s*8\s*\*\s*1024\s*\*\s*1024') {
  throw 'The Weasel transaction buffer must match the 8 MiB ABI result budget.'
}
foreach ($required in @(
    'buffer(new char[bs])',
    'UINT received_wchars',
    'bool PrepareTransactionBuffer() noexcept',
    'bool TryWrite(const std::wstring& cnt)',
    'const size_t capacity = _SendBufferSizeW()',
    'cnt.size() > capacity - used',
    'stream.write(cnt.data()',
    'ctx->has_body = true',
    'Transact(Msg& msg, bool* request_sent = nullptr)',
    '*request_sent = false',
    '*request_sent = true',
    'ctx->received_wchars = 0',
    'return handler((LPWSTR)ctx->buffer.get(), ctx->received_wchars)')) {
  if (-not $pipeHeader.Contains($required)) {
    throw "PipeChannel direct-buffer ownership is missing a contract token: $required"
  }
}
if ($pipeHeader -match
      'std::make_unique\s*<\s*char\[\]\s*>\s*\(\s*bs\s*\)|' +
      'buff_size\s*\*\s*sizeof\(char\)\s*/\s*sizeof\(wchar_t\)') {
  throw ('The 8 MiB receive store must not be value-initialized or exposed at ' +
         'full capacity when only a shorter response was received.')
}
$requestFalseIndex = $pipeHeader.IndexOf('*request_sent = false')
$sendRequestIndex = $pipeHeader.IndexOf('_Send(*phandle, msg)')
$requestTrueIndex = $pipeHeader.IndexOf(
  '*request_sent = true', $sendRequestIndex)
$receiveResponseIndex = $pipeHeader.IndexOf(
  'return _ReceiveResponse()', $requestTrueIndex)
if ($requestFalseIndex -lt 0 -or $sendRequestIndex -lt 0 -or
    $requestTrueIndex -lt 0 -or $receiveResponseIndex -lt 0 -or
    -not ($requestFalseIndex -lt $sendRequestIndex -and
          $sendRequestIndex -lt $requestTrueIndex -and
          $requestTrueIndex -lt $receiveResponseIndex)) {
  throw ('Transact must publish request_sent only after the write succeeds and ' +
         'before the response read can become uncertain.')
}

$responseParser = Get-ResultingHunkText -Patch $patchText -FunctionName 'ResponseParser::operator()'
foreach ($required in @(
    'bool complete = false',
    'if (line == L".")',
    'complete = true',
    'continue',
    'return complete')) {
  if (-not $responseParser.Contains($required)) {
    throw "ResponseParser must consume ordered complete documents in one transaction: $required"
  }
}
$committer = Get-ResultingHunkText -Patch $patchText -FunctionName 'Committer::Store'
if (-not $committer.Contains('p_commit->append(unescape_string(value))') -or
    $committer.Contains('*m_pTarget->p_commit =')) {
  throw 'Recovered and current commits must be appended in exact wire order.'
}

foreach ($functionName in @(
    'ServerImpl::OnHighlightCandidateOnCurrentPage',
    'ServerImpl::OnChangePage')) {
  $resulting = Get-ResultingHunkText -Patch $patchText -FunctionName $functionName
  if ($resulting -notmatch '(?s)return\s+m_pRequestHandler->.*?\?\s*1\s*:\s*0\s*;') {
    throw "$functionName must propagate response-ready to the TSF client."
  }
}

$receiveResulting = Get-ResultingHunkText -Patch $patchText -FunctionName '_ReceiveResponse'
$clearIndex = $receiveResulting.IndexOf(
  "*reinterpret_cast<wchar_t*>(ctx->buffer.get()) = L'\0'")
$receiveIndex = $receiveResulting.IndexOf('_Receive(*phandle, &result, sizeof(result))')
if ($clearIndex -lt 0 -or $receiveIndex -lt 0 -or $clearIndex -gt $receiveIndex) {
  throw 'Every IPC transaction must terminate the shared response body before receiving a possibly body-less reply.'
}
if ($receiveResulting.IndexOf('ctx->received_wchars = 0') -lt 0) {
  throw 'A body-less response must publish an exact zero received length.'
}
if ($receiveResulting.Contains('memset(ctx->buffer.get(), 0, buff_size)')) {
  throw 'Ordinary key transactions must not memset the full 8 MiB response capacity.'
}

$readPipeResulting = Get-ResultingHunkText -Patch $patchText -FunctionName 'PipeChannelBase::_Receive'
foreach ($required in @(
    'ctx->received_wchars = 0',
    'ctx->buffer.get() + body_read',
    "= L'\0'",
    'static_cast<UINT>(body_read / sizeof(wchar_t) + 1u)')) {
  if (-not $readPipeResulting.Contains($required)) {
    throw "Pipe reads must expose only the received and terminated body length: $required"
  }
}
$terminateBodyIndex = $readPipeResulting.IndexOf(
  'ctx->buffer.get() + body_read')
$publishBodyLengthIndex = $readPipeResulting.IndexOf(
  'ctx->received_wchars =', $terminateBodyIndex)
if ($terminateBodyIndex -lt 0 -or $publishBodyLengthIndex -lt 0 -or
    $terminateBodyIndex -gt $publishBodyLengthIndex -or
    $readPipeResulting.Contains('memset(ctx->buffer.get(), 0, buff_size)')) {
  throw 'The shared response buffer must be terminated before its exact live wchar count is published.'
}

$candidateSelect = Get-ResultingHunkText -Patch $patchText -FunctionName 'WeaselTSF::_SelectCandidateOnCurrentPage'
if ($candidateSelect -match 'VK_SELECT|SendInput' -or
    $candidateSelect -notmatch
      '(?s)if\s*\(\s*m_client\.SelectCandidateOnCurrentPage\(index\)\s*\)\s*_UpdateComposition') {
  throw 'Candidate selection must update from its IPC response without a synthetic key.'
}

$destroyResulting = Get-ResultingHunkText -Patch $patchText -FunctionName 'RimeWithWeaselHandler::_DestroyAbiContexts'
$resetIndex = $destroyResulting.IndexOf('m_abi_result.Reset()')
$destroyIndex = $destroyResulting.IndexOf('_DestroyEngineContext')
$clearIndex = $destroyResulting.IndexOf('m_session_status_map.clear()')
if ($resetIndex -lt 0 -or $destroyIndex -lt 0 -or $clearIndex -lt 0 -or
    -not ($resetIndex -lt $destroyIndex -and $destroyIndex -lt $clearIndex)) {
  throw 'Shutdown must reset the result lease, destroy all contexts, then clear the map.'
}

$finalizeResulting = Get-ResultingHunkText -Patch $patchText -FunctionName 'RimeWithWeaselHandler::Finalize'
if ($finalizeResulting.IndexOf('_DestroyAbiContexts()') -lt 0 -or
    $finalizeResulting.IndexOf('_DestroyAbiContexts()') -gt
      $finalizeResulting.IndexOf('m_engine_host.Unload()')) {
  throw 'Finalize must release results and contexts before unloading the engine.'
}
if ($finalizeResulting.LastIndexOf('_ClearNotifications()') -lt
    $finalizeResulting.IndexOf('m_engine_host.Unload()')) {
  throw 'Finalize must clear queues again after Unload has joined the last callback.'
}

$finalizeHandlerResulting = Get-ResultingHunkText `
  -Patch $patchText `
  -FunctionName 'ServerImpl::_FinalizeRequestHandler'
$finalizeLockIndex = $finalizeHandlerResulting.IndexOf(
  'std::lock_guard guard(g_api_mutex)')
$exchangeHandlerIndex = $finalizeHandlerResulting.IndexOf(
  'std::exchange(m_pRequestHandler, nullptr)')
$handlerFinalizeIndex = $finalizeHandlerResulting.IndexOf(
  'handler->Finalize()')
if ($finalizeLockIndex -lt 0 -or $exchangeHandlerIndex -lt 0 -or
    $handlerFinalizeIndex -lt 0 -or
    -not ($finalizeLockIndex -lt $exchangeHandlerIndex -and
          $exchangeHandlerIndex -lt $handlerFinalizeIndex)) {
  throw ('Final shutdown must hold the ABI lock and unpublish the handler ' +
         'before Finalize can unload its engine.')
}

$serverRunResulting = Get-ResultingHunkText `
  -Patch $patchText `
  -FunctionName 'ServerImpl::Run'
foreach ($required in @(
    '[this, listener]()',
    'std::lock_guard guard(g_api_mutex)',
    '!m_pRequestHandler',
    'std::exception_ptr run_error',
    'run_error = std::current_exception()',
    '_BeginShutdown()',
    'PeekMessage(',
    'MsgWaitForMultipleObjects(',
    'std::rethrow_exception(run_error)',
    'WM_FAMO_BEGIN_SHUTDOWN')) {
  if (-not $serverRunResulting.Contains($required)) {
    throw "The listener and normal shutdown path must own stable state: $required"
  }
}
if ($serverRunResulting.Contains('&listener')) {
  throw 'The accept thread must own its listener callback instead of referencing Run stack storage.'
}

$pipeListenResulting = Get-ResultingHunkText `
  -Patch $patchText `
  -FunctionName 'PipeServer::Listen'
foreach ($required in @(
    'PipeServer::Listen(ServerHandler handler)',
    'stopping_.load(',
    '[this, pipe, handler, completed]',
    '_ReapWorkers(false)',
    'workers_.emplace_back()',
    'workers_.back().thread = std::make_unique<boost::thread>',
    'completed->store(true, std::memory_order_release)')) {
  if (-not $pipeListenResulting.Contains($required)) {
    throw "Accepted pipe workers must own their callback and participate in shutdown: $required"
  }
}
if ($pipeListenResulting.Contains('&handler')) {
  throw 'Accepted pipe workers must never reference accept-thread callback storage.'
}
if ($pipeListenResulting.Contains('.detach()')) {
  throw 'Accepted pipe workers must remain joinable through shutdown.'
}

$waitForWorkersResulting = Get-ResultingHunkText `
  -Patch $patchText `
  -FunctionName 'PipeServer::WaitForWorkers'
if (-not $waitForWorkersResulting.Contains('_ReapWorkers(true)')) {
  throw 'Shutdown must join all accepted pipe workers.'
}

$reapWorkersResulting = Get-ResultingHunkText `
  -Patch $patchText `
  -FunctionName 'PipeServer::_ReapWorkers'
foreach ($required in @(
    'it->completed->load(std::memory_order_acquire)',
    'if (!join_all && !completed)',
    'it->thread->joinable()',
    'it->thread->join()',
    'workers_.erase(it)')) {
  if (-not $reapWorkersResulting.Contains($required)) {
    throw "Worker cleanup must join completed threads before erasing them: $required"
  }
}
if ($reapWorkersResulting.Contains('.detach()')) {
  throw 'Worker cleanup must never detach an accepted pipe thread.'
}

$beginShutdownResulting = Get-ResultingHunkText `
  -Patch $patchText `
  -FunctionName 'ServerImpl::_BeginShutdown'
$stopAcceptIndex = $beginShutdownResulting.IndexOf('channel->StopAccepting()')
$interruptIndex = $beginShutdownResulting.IndexOf('pipeThread->interrupt()')
$joinIndex = $beginShutdownResulting.IndexOf('pipeThread->join()')
$waitWorkersIndex = $beginShutdownResulting.IndexOf(
  'channel->WaitForWorkers()')
$finishPostIndex = $beginShutdownResulting.IndexOf(
  'WM_FAMO_FINISH_SHUTDOWN')
$missingShutdownStep = @(
  @(
    $stopAcceptIndex
    $interruptIndex
    $joinIndex
    $waitWorkersIndex
    $finishPostIndex
  ) | Where-Object { $_ -lt 0 }
)
if ($missingShutdownStep.Count -gt 0 -or
    -not ($stopAcceptIndex -lt $interruptIndex -and
          $interruptIndex -lt $joinIndex -and
          $joinIndex -lt $waitWorkersIndex -and
          $waitWorkersIndex -lt $finishPostIndex)) {
  throw ('Shutdown must close admission, join the accept thread, drain every ' +
         'accepted worker, then notify the still-pumping UI thread.')
}

$finishShutdownResulting = Get-ResultingHunkText `
  -Patch $patchText `
  -FunctionName 'ServerImpl::OnFinishShutdown'
$coordinatorJoinIndex = $finishShutdownResulting.IndexOf(
  'shutdownThread->join()')
$finishFinalizeIndex = $finishShutdownResulting.IndexOf(
  '_FinalizeRequestHandler()')
$postQuitIndex = $finishShutdownResulting.IndexOf('PostQuitMessage(0)')
if ($coordinatorJoinIndex -lt 0 -or $finishFinalizeIndex -lt 0 -or
    $postQuitIndex -lt 0 -or
    -not ($coordinatorJoinIndex -lt $finishFinalizeIndex -and
          $finishFinalizeIndex -lt $postQuitIndex)) {
  throw 'The UI loop may quit only after the drain coordinator joins and the handler finalizes.'
}

$stopResulting = Get-ResultingHunkText `
  -Patch $patchText `
  -FunctionName 'int ServerImpl::Stop()'
if (-not $stopResulting.Contains('channel->StopAccepting()') -or
    -not $stopResulting.Contains('WM_FAMO_BEGIN_SHUTDOWN') -or
    $stopResulting.Contains('join()') -or
    $stopResulting.Contains('WM_QUIT')) {
  throw 'Stop must only close admission and request asynchronous drain; a pipe worker must never join itself.'
}

$pipeProcessResulting = Get-ResultingHunkText `
  -Patch $patchText `
  -FunctionName 'PipeServer::_ProcessPipeThread'
$receiveRequestIndex = $pipeProcessResulting.IndexOf(
  '_Receive(pipe, &msg, sizeof(msg))')
$postReceiveStopIndex = $pipeProcessResulting.IndexOf(
  'stopping_.load(', $receiveRequestIndex)
$dispatchHandlerIndex = $pipeProcessResulting.IndexOf(
  'handler(', $receiveRequestIndex)
if ($receiveRequestIndex -lt 0 -or $postReceiveStopIndex -lt 0 -or
    $dispatchHandlerIndex -lt 0 -or
    -not ($receiveRequestIndex -lt $postReceiveStopIndex -and
          $postReceiveStopIndex -lt $dispatchHandlerIndex)) {
  throw 'A request received during shutdown must be discarded before entering the handler.'
}
$workerCatchIndex = $pipeProcessResulting.LastIndexOf('catch (...)')
$closeWorkerPipeIndex = $pipeProcessResulting.LastIndexOf(
  '_FinalizePipe(pipe)')
if ($workerCatchIndex -lt 0 -or $closeWorkerPipeIndex -lt 0 -or
    $closeWorkerPipeIndex -lt $workerCatchIndex) {
  throw 'Every accepted pipe must close after either an exception or a normal shutdown exit.'
}

foreach ($functionName in @(
    'ServerImpl::OnColorChange',
    'ServerImpl::SetOption',
    'ServerImpl::OnDeferredOptions',
    'ServerImpl::OnUiTasks')) {
  $resulting = Get-ResultingHunkText -Patch $patchText -FunctionName $functionName
  if (-not $resulting.Contains(
      'std::unique_lock lock(g_api_mutex, std::try_to_lock)') -or
      -not $resulting.Contains('if (!lock.owns_lock())') -or
      -not $resulting.Contains('::PostMessage(')) {
    throw "$functionName must retry without blocking the UI thread on the worker ABI lock."
  }
}

$windowCommandResulting = Get-ResultingHunkText `
  -Patch $patchText `
  -FunctionName 'LRESULT ServerImpl::OnCommand(UINT'
if ($windowCommandResulting -notmatch
    '(?s)std::unique_lock\s+lock\(g_api_mutex,\s*std::try_to_lock\).*?' +
    'if\s*\(\s*!lock\.owns_lock\(\)\s*\).*?PostMessage\(.*?WM_COMMAND.*?' +
    '_DispatchCommandLocked\(') {
  throw 'The window command entry must retry asynchronously until it can serialize with pipe ABI work.'
}
$pipeCommandResulting = Get-ResultingHunkText `
  -Patch $patchText `
  -FunctionName 'DWORD ServerImpl::OnCommand(WEASEL_IPC_COMMAND'
if (-not $pipeCommandResulting.Contains(
      '::PostMessage(m_hWnd, WM_COMMAND, wParam, lParam)') -or
    $pipeCommandResulting.Contains('_DispatchCommandLocked(') -or
    $pipeCommandResulting.Contains(
      'OnCommand(uMsg, wParam, lParam')) {
  throw 'A pipe tray command must post UI work instead of re-entering the window handler under the ABI lock.'
}

$endSessionResulting = Get-ResultingHunkText `
  -Patch $patchText `
  -FunctionName 'ServerImpl::OnEndSystemSession'
if ($endSessionResulting -notmatch
      '(?s)if\s*\(\s*wParam\s*\)\s*Stop\(\)' -or
    $endSessionResulting.Contains('Finalize()')) {
  throw 'WM_ENDSESSION(FALSE) must be a no-op and TRUE must request the shared drain path.'
}

$serverAppResulting = Get-ResultingFileText `
  -Patch $patchText `
  -FilePath 'WeaselServer/WeaselServerApp.cpp'
foreach ($required in @(
    'm_server.SetOption(0, opt, val)',
    'm_server.PostUiTask(',
    'm_server.SetRequestHandler(nullptr)',
    'int ret = m_server.Run()')) {
  if (-not $serverAppResulting.Contains($required)) {
    throw "App callbacks must route engine/UI work through the serialized server: $required"
  }
}
if ($serverAppResulting.Contains('m_handler->SetOption(0, opt, val)') -or
    $serverAppResulting.Contains('m_handler->Finalize()')) {
  throw 'The app message loop must not bypass server serialization for SetOption or Finalize.'
}

Write-Host "PASS: engine-abi.patch added lines are ABI-only."
