<#
.SYNOPSIS
  Freeze one stable TSF Bridge DLL into an independently versioned artifact.
#>
param(
  [Parameter(Mandatory)]
  [string] $BridgeDll,
  [Parameter(Mandatory)]
  [string] $Output,
  [Parameter(Mandatory)]
  [ValidateRange(1, 65535)]
  [int] $BridgeAbi,
  [Parameter(Mandatory)]
  [ValidateRange(1, 65535)]
  [int] $ProtocolMin,
  [Parameter(Mandatory)]
  [ValidateRange(1, 65535)]
  [int] $ProtocolMax
)

$ErrorActionPreference = 'Stop'
if ($ProtocolMax -lt $ProtocolMin) {
  throw 'ProtocolMax 必须大于或等于 ProtocolMin。'
}
if (-not (Test-Path -LiteralPath $BridgeDll -PathType Leaf)) {
  throw "Bridge DLL 不存在：$BridgeDll"
}

$outputFull = [IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $outputFull) {
  $existing = Join-Path $outputFull 'FamoTextService.dll'
  if (Test-Path -LiteralPath $existing) {
    $existingHash = (Get-FileHash -LiteralPath $existing -Algorithm SHA256).Hash
    $sourceHash = (Get-FileHash -LiteralPath $BridgeDll -Algorithm SHA256).Hash
    if ($existingHash -ne $sourceHash) {
      throw '拒绝用不同字节覆盖既有 Bridge ABI artifact；请提升 BridgeAbi 并使用新目录。'
    }
  }
} else {
  New-Item -ItemType Directory -Path $outputFull | Out-Null
}

$target = Join-Path $outputFull 'FamoTextService.dll'
if ([IO.Path]::GetFullPath($BridgeDll) -ne [IO.Path]::GetFullPath($target)) {
  Copy-Item -LiteralPath $BridgeDll -Destination $target -Force
}
$item = Get-Item -LiteralPath $target
$fileMajor = $item.VersionInfo.FileMajorPart
if ($fileMajor -ne $BridgeAbi) {
  throw "Bridge DLL 文件主版本 $fileMajor 与 BridgeAbi $BridgeAbi 不一致。"
}
$hash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
$manifest = @(
  'format=1'
  'identity=Stable'
  "bridge_abi=$BridgeAbi"
  "protocol_min=$ProtocolMin"
  "protocol_max=$ProtocolMax"
  "file=FamoTextService.dll|$($item.Length)|$hash"
)
[IO.File]::WriteAllLines(
  (Join-Path $outputFull 'bridge-manifest.txt'),
  $manifest,
  [Text.UTF8Encoding]::new($false))

Write-Host "OK -> $outputFull"
Write-Host "Bridge ABI = $BridgeAbi; protocol = $ProtocolMin..$ProtocolMax"
Write-Host "SHA256 = $hash"
