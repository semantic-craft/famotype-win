param(
  [string]$BoundedPatch = (Join-Path $PSScriptRoot '..\features\bounded-ipc-connect.patch'),
  [string]$EnginePatch = (Join-Path $PSScriptRoot '..\features\engine-abi.patch')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

foreach ($path in @($BoundedPatch, $EnginePatch)) {
  if (-not (Test-Path -LiteralPath $path)) {
    throw "Missing patch: $path"
  }
}

$bounded = Get-Content -LiteralPath $BoundedPatch -Raw
$engine = Get-Content -LiteralPath $EnginePatch -Raw

function Get-ResultingHunkText {
  param(
    [Parameter(Mandatory = $true)] [string] $Patch,
    [Parameter(Mandatory = $true)] [string] $Needle
  )

  $hunk = [regex]::Matches(
    $Patch,
    '(?ms)^@@[^\r\n]*\r?\n(?<body>.*?)(?=^@@|^diff --git|\z)') |
    Where-Object { $_.Value.Contains($Needle) } |
    Select-Object -First 1
  if ($null -eq $hunk) {
    throw "Patch must contain a hunk for: $Needle"
  }
  return (($hunk.Groups['body'].Value -split '\r?\n') |
    Where-Object {
      ($_.StartsWith('+') -and -not $_.StartsWith('+++')) -or
      $_.StartsWith(' ')
    } |
    ForEach-Object { $_.Substring(1) }) -join "`n"
}

if ($bounded -match '(?m)^\+.*::CancelIo\(') {
  throw 'Bounded pipe I/O must use operation-specific CancelIoEx, not CancelIo.'
}
foreach ($token in @(
    '::CancelIoEx(pipe, &overlapped)',
    'struct FamoPipeIoResult',
    'return {transferred, ERROR_SUCCESS}',
    'return {transferred, ERROR_MORE_DATA}',
    'completion_error == ERROR_OPERATION_ABORTED',
    'cancel_error == ERROR_NOT_FOUND')) {
  if (-not $bounded.Contains($token)) {
    throw "Bounded pipe cancellation is missing terminal-drain token: $token"
  }
}

$cancelResult = Get-ResultingHunkText `
  -Patch $bounded `
  -Needle 'FamoCancelPipeIoAndWait(HANDLE pipe'
if ($cancelResult -notmatch
      '(?s)GetOverlappedResult\(\s*pipe,\s*&overlapped,\s*&transferred,\s*TRUE\).*?' +
      'return\s*\{\s*transferred,\s*ERROR_SUCCESS\s*\}' -or
    $cancelResult -notmatch
      '(?s)completion_error\s*==\s*ERROR_MORE_DATA.*?' +
      'return\s*\{\s*transferred,\s*ERROR_MORE_DATA\s*\}' -or
    $cancelResult.IndexOf('_ThrowCode(error_to_report)') -lt
      $cancelResult.IndexOf('return {transferred, ERROR_MORE_DATA}')) {
  throw ('CancelIoEx losing the timeout race must return the natural write/read ' +
         'completion, including a valid ERROR_MORE_DATA header, before reporting ' +
         'a real timeout.')
}

$boundedIoResult = Get-ResultingHunkText `
  -Patch $bounded `
  -Needle 'FamoFinishPipeIo(HANDLE pipe'
$timeoutDrains = [regex]::Matches(
  $boundedIoResult,
  'FamoCancelPipeIoAndWait\(\s*pipe,\s*overlapped,\s*ERROR_SEM_TIMEOUT\)')
$failedWaitDrains = [regex]::Matches(
  $boundedIoResult,
  '(?s)wait\s*==\s*WAIT_FAILED.*?GetLastError\(\).*?' +
  'FamoCancelPipeIoAndWait\(\s*pipe,\s*overlapped,\s*wait_error\)')
if ($timeoutDrains.Count -ne 2 -or $failedWaitDrains.Count -lt 2) {
  throw 'Both bounded write and read helpers must cancel and drain on timeout and WAIT_FAILED.'
}
foreach ($required in @(
    'const FamoPipeIoResult completion =',
    'return completion.transferred',
    'error = completion.error',
    'return error == ERROR_MORE_DATA')) {
  if (-not $bounded.Contains($required)) {
    throw "Bounded callers must consume the final completion instead of misclassifying it: $required"
  }
}
$serverConnectResult = Get-ResultingHunkText `
  -Patch $bounded `
  -Needle 'ConnectNamedPipe(pipe, &overlapped)'
if ($serverConnectResult -notmatch
    '(?s)ConnectNamedPipe\(pipe,\s*&overlapped\).*?' +
    'WaitForSingleObject\(overlapped\.hEvent,\s*kFamoPipeConnectBudgetMs\).*?' +
    'wait\s*==\s*WAIT_TIMEOUT\s*\?\s*ERROR_SEM_TIMEOUT.*?' +
    'wait\s*==\s*WAIT_FAILED.*?' +
    'FamoCancelPipeIoAndWait\(\s*pipe,\s*overlapped,\s*wait_error\)') {
  throw ('The overlapped server accept must be bounded so shutdown can join it, ' +
         'and every timeout/WAIT_FAILED path must cancel and drain before stack teardown.')
}
if ($serverConnectResult -notmatch
    '(?s)HANDLE\s+pipe\s*=\s*CreateNamedPipe\(.*?' +
    'try\s*\{.*?ConnectNamedPipe\(pipe,\s*&overlapped\).*?' +
    '\}\s*catch\s*\(\.\.\.\)\s*\{\s*_FinalizePipe\(pipe\);\s*throw;\s*\}') {
  throw 'Every post-CreateNamedPipe exception must close the accepted server handle.'
}
foreach ($required in @(
    'struct ScopedEvent',
    '~ScopedEvent()',
    '::CloseHandle(handle)')) {
  if (-not $bounded.Contains($required)) {
    throw "Every overlapped I/O event must have a no-leak owner: $required"
  }
}

$connectResult = Get-ResultingHunkText -Patch $bounded -Needle 'WaitNamedPipe(name, wait_ms)'
foreach ($retryable in @(
    'wait_error != ERROR_SEM_TIMEOUT',
    'wait_error != ERROR_FILE_NOT_FOUND',
    'wait_error != ERROR_PIPE_BUSY')) {
  if (-not $connectResult.Contains($retryable)) {
    throw "Pipe startup/busy race must remain retryable inside the budget: $retryable"
  }
}
if (-not $connectResult.Contains('_ThrowCode(wait_error)')) {
  throw 'Unexpected WaitNamedPipe errors must still fail fast.'
}
$setModeIndex = $connectResult.IndexOf(
  'if (!SetNamedPipeHandleState(pipe, &mode, NULL, NULL))')
$closeBadClientPipeIndex = $connectResult.IndexOf(
  '_FinalizePipe(pipe)', $setModeIndex)
$throwBadClientPipeIndex = $connectResult.IndexOf(
  '_ThrowCode(error)', $closeBadClientPipeIndex)
if ($setModeIndex -lt 0 -or $closeBadClientPipeIndex -lt 0 -or
    $throwBadClientPipeIndex -lt 0 -or
    -not ($setModeIndex -lt $closeBadClientPipeIndex -and
          $closeBadClientPipeIndex -lt $throwBadClientPipeIndex)) {
  throw 'A connected client pipe must be closed before SetNamedPipeHandleState failure escapes.'
}

$boundedSend = Get-ResultingHunkText -Patch $bounded -Needle '_WritePipe(*retry_pipe'
if ($boundedSend -notmatch
      '(?s)try\s*\{\s*try\s*\{\s*_WritePipe\(pipe,\s*data_sz,\s*pbuff\);.*?' +
      'catch\s*\(DWORD\s+ex\).*?_Reconnect\(\);.*?' +
      'HANDLE\*\s+retry_pipe\s*=\s*_GetPipeHandle\(\);.*?' +
      '_WritePipe\(\*retry_pipe,\s*data_sz,\s*pbuff\);.*?' +
      '\}\s*catch\s*\(\.\.\.\)\s*\{\s*.*?ClearBufferStream\(\);\s*throw;' -or
    $boundedSend -notmatch
      '(?s)\}\s*catch\s*\(\.\.\.\).*?\}\s*ClearBufferStream\(\);\s*\}') {
  throw 'Bounded _Send must use the replacement handle and clear state after initial, reconnect, retry, or success exits.'
}
$reconnectResult = Get-ResultingHunkText -Patch $bounded -Needle 'PipeChannelBase::_Reconnect'
if ($reconnectResult -notmatch
    '(?s)if\s*\(\s*!_Ensure\(\)\s*\)\s*\{\s*_ThrowCode\(ERROR_PIPE_NOT_CONNECTED\)') {
  throw '_Reconnect must turn an unsuccessful _Ensure into an explicit failure.'
}

$pipeHunk = [regex]::Matches(
  $engine,
  '(?ms)^@@[^\r\n]*class PipeChannel[^\r\n]*\r?\n(?<body>.*?)(?=^@@|^diff --git|\z)') |
  Where-Object {
    $_.Groups['body'].Value.Contains('_WritePipe(*retry_pipe') -and
    $_.Groups['body'].Value.Contains('_ReceiveResponse()')
  } |
  Select-Object -First 1
if ($null -eq $pipeHunk) {
  throw 'engine-abi.patch must contain the PipeChannel send/receive hunk.'
}
$pipeResult = (($pipeHunk.Groups['body'].Value -split '\r?\n') |
  Where-Object {
    ($_.StartsWith('+') -and -not $_.StartsWith('+++')) -or
    $_.StartsWith(' ')
  } |
  ForEach-Object { $_.Substring(1) }) -join "`n"

if ($pipeResult -notmatch
      '(?s)_WritePipe\(\*retry_pipe,\s*data_sz,\s*pbuff\);.*?' +
      'catch\s*\(\.\.\.\)\s*\{\s*.*?ClearTransactionBuffer\(\);\s*throw;' -or
    $pipeResult -notmatch
      '(?s)\}\s*catch\s*\(\.\.\.\).*?\}\s*ClearTransactionBuffer\(\);\s*\}') {
  throw 'PipeChannel::_Send must clear request/response state after success and every initial/retry failure.'
}
if ($pipeResult -match
    '(?s)_Reconnect\(\);\s*_WritePipe\(pipe,\s*data_sz,\s*pbuff\)') {
  throw 'PipeChannel::_Send must never retry the closed by-value handle.'
}

$clearIndex = $pipeResult.IndexOf(
  "*reinterpret_cast<wchar_t*>(ctx->buffer.get()) = L'\0'")
$readIndex = $pipeResult.IndexOf('_Receive(*phandle, &result, sizeof(result))')
if ($clearIndex -lt 0 -or $readIndex -lt 0 -or $clearIndex -gt $readIndex) {
  throw 'Every response receive must terminate the shared body before a body-less reply can arrive.'
}
if ($pipeResult.Contains('memset(ctx->buffer.get(), 0, buff_size)')) {
  throw 'A key transaction must not clear the entire 8 MiB response capacity.'
}

$boundedReceive = Get-ResultingHunkText `
  -Patch $bounded `
  -Needle 'const DWORD body_read'
foreach ($required in @(
    'body_read % sizeof(wchar_t) != 0',
    'body_read > buff_size - sizeof(wchar_t)',
    'ctx->buffer.get() + body_read',
    "= L'\0'")) {
  if (-not $boundedReceive.Contains($required)) {
    throw "A shorter body must be length-checked and terminated without exposing stale tail bytes: $required"
  }
}
if ($boundedReceive.Contains('memset(ctx->buffer.get(), 0, buff_size)')) {
  throw 'Bounded receive must terminate actual bytes, not memset the whole 8 MiB buffer.'
}

Write-Host 'PASS: bounded IPC cancellation and stale-buffer contracts.'
