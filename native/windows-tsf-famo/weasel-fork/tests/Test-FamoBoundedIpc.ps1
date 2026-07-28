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
    '::GetOverlappedResult(pipe, &overlapped, &transferred, TRUE)',
    'completion_error == ERROR_OPERATION_ABORTED',
    'cancel_error == ERROR_NOT_FOUND')) {
  if (-not $bounded.Contains($token)) {
    throw "Bounded pipe cancellation is missing terminal-drain token: $token"
  }
}

$timeoutDrains = [regex]::Matches(
  $bounded,
  'FamoCancelPipeIoAndWait\(pipe,\s*overlapped,\s*ERROR_SEM_TIMEOUT\)')
$failedWaitDrains = [regex]::Matches(
  $bounded,
  '(?s)wait\s*==\s*WAIT_FAILED.*?GetLastError\(\).*?' +
  'FamoCancelPipeIoAndWait\(pipe,\s*overlapped,\s*wait_error\)')
if ($timeoutDrains.Count -ne 2 -or $failedWaitDrains.Count -lt 2) {
  throw 'Both bounded write and read helpers must cancel and drain on timeout and WAIT_FAILED.'
}
if ($bounded -notmatch
    '(?s)ConnectNamedPipe\(pipe,\s*&overlapped\).*?' +
    'WaitForSingleObject\(overlapped\.hEvent,\s*INFINITE\).*?' +
    'wait\s*==\s*WAIT_FAILED.*?' +
    'FamoCancelPipeIoAndWait\(pipe,\s*overlapped,\s*wait_error\)') {
  throw 'The overlapped server-connect WAIT_FAILED path must also cancel and drain before stack teardown.'
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

$clearIndex = $pipeResult.IndexOf('memset(ctx->buffer.get(), 0, buff_size)')
$readIndex = $pipeResult.IndexOf('_Receive(*phandle, &result, sizeof(result))')
if ($clearIndex -lt 0 -or $readIndex -lt 0 -or $clearIndex -gt $readIndex) {
  throw 'Every response receive must clear the shared body before a body-less reply can arrive.'
}

Write-Host 'PASS: bounded IPC cancellation and stale-buffer contracts.'
