#requires -version 5
<#
.SYNOPSIS
  取证 %AppData%\Rime 全程只读未被写（S3.3 / 全局护栏）。

.DESCRIPTION
  形态甲铁律：法墨业务运行不得把 %AppData%\Rime 作为配置目录、不得写入，
  仅可为前后对比做只读 hash 枚举。用法：
    .\check-rime-untouched.ps1 -Snapshot before.json     # 操作前快照
    <做 seed / 部署等操作>
    .\check-rime-untouched.ps1 -Snapshot after.json
    .\check-rime-untouched.ps1 -Before before.json -After after.json   # 应 no diff
#>
param(
  [string] $Snapshot,
  [string] $Before,
  [string] $After
)

$ErrorActionPreference = 'Stop'
$rime = Join-Path $env:APPDATA 'Rime'

function Get-RimeHashes {
  if (-not (Test-Path $rime)) { return @() }
  Get-ChildItem $rime -Recurse -File | ForEach-Object {
    [pscustomobject]@{
      Path = $_.FullName.Substring($rime.Length).TrimStart('\')
      Hash = (Get-FileHash $_.FullName -Algorithm SHA256).Hash
    }
  } | Sort-Object Path
}

if ($Snapshot) {
  $h = Get-RimeHashes
  $h | ConvertTo-Json -Depth 3 | Set-Content $Snapshot -Encoding UTF8
  Write-Output "snapshot -> $Snapshot  ($($h.Count) files under $rime)"
  return
}

if ($Before -and $After) {
  $a = Get-Content $Before -Raw | ConvertFrom-Json
  $b = Get-Content $After -Raw | ConvertFrom-Json
  $sa = ($a | ForEach-Object { "$($_.Path)=$($_.Hash)" })
  $sb = ($b | ForEach-Object { "$($_.Path)=$($_.Hash)" })
  $diff = Compare-Object $sa $sb
  if (-not $diff) { Write-Output "RIME UNTOUCHED: PASS (no diff, $($a.Count) files)" }
  else { Write-Output "RIME CHANGED: FAIL"; $diff | Format-Table -AutoSize | Out-String | Write-Output; exit 1 }
  return
}

Write-Output "用法: -Snapshot <file> | -Before <file> -After <file>"
