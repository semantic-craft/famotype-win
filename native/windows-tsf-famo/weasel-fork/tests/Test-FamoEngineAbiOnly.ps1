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
    '_DeliverPendingResponse(ipc_id, session_status, eat)',
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
$pendingDeliveryIndex = $executeAction.IndexOf(
  'if (session_status->pending_response_result &&')
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
$deliverRecoveredResultIndex = $executeAction.IndexOf(
  '_DeliverPendingResponse(ipc_id, session_status, eat)', $clearPendingIndex)
$businessDispatchIndex = $executeAction.IndexOf(
  'm_engine_host.ExecuteActionRecovering(')
if ($pendingDeliveryIndex -lt 0 -or
    $pendingBranchIndex -lt $pendingDeliveryIndex -or
    $recoverDispatchIndex -lt $pendingBranchIndex -or
    $failedRecoveryIndex -lt $recoverDispatchIndex -or
    $failOpenIndex -lt $failedRecoveryIndex -or
    $pendingReturnIndex -lt $failOpenIndex -or
    $adoptRecoveredResultIndex -lt $pendingReturnIndex -or
    $clearPendingIndex -lt $adoptRecoveredResultIndex -or
    $deliverRecoveredResultIndex -lt $clearPendingIndex -or
    $businessDispatchIndex -lt $deliverRecoveredResultIndex) {
  throw ('Pending RECOVER must finish within the current callback: on failure ' +
         'the last physical input fails open before business dispatch; on ' +
         'success the exact final-result lease is retained and delivered before ' +
         'the current action runs.')
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
$selectUpdate = $selectResulting.IndexOf('_UpdateUI(ipc_id)')
if ($selectExecute -lt 0 -or $selectRespond -lt 0 -or $selectUpdate -lt 0 -or
    -not ($selectExecute -lt $selectRespond -and $selectRespond -lt $selectUpdate)) {
  throw 'Selection must execute once, respond with that same result, then refresh UI.'
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
    'pending_response_result.Reset()',
    'catch (...)',
    'engine-owned lease remains the exact retry anchor',
    'Commit itself violates the bounded ABI/pipe contract')) {
  if (-not $respondResulting.Contains($required)) {
    throw "_DeliverPendingResponse must preserve exact, bounded, no-unwind delivery: $required"
  }
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
    'bool TryWrite(const std::wstring& cnt)',
    'const size_t capacity = _SendBufferSizeW()',
    'cnt.size() > capacity - used',
    'stream.write(cnt.data()',
    'ctx->has_body = true')) {
  if (-not $pipeHeader.Contains($required)) {
    throw "PipeChannel must reject a whole response before any partial append: $required"
  }
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
$clearIndex = $receiveResulting.IndexOf('memset(ctx->buffer.get(), 0, buff_size)')
$receiveIndex = $receiveResulting.IndexOf('_Receive(*phandle, &result, sizeof(result))')
if ($clearIndex -lt 0 -or $receiveIndex -lt 0 -or $clearIndex -gt $receiveIndex) {
  throw 'Every IPC transaction must clear the shared response body before receiving a possibly body-less reply.'
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

Write-Host "PASS: engine-abi.patch added lines are ABI-only."
