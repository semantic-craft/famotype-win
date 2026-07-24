#requires -version 5
<#
.SYNOPSIS
  首启 seed 法墨引擎配置（S3.3）：famo-config/payload → %LOCALAPPDATA%\Famo。

.DESCRIPTION
  把内置 rime-ice(雾凇 rime_ice) + 极点五笔 + 法墨 overlay 的 payload 拷到 %LOCALAPPDATA%\Famo，
  使 librime 引擎从 Famo 自有目录读配置（配 weasel-fork 的 RimeUserDir 品牌键）。
  只写 %LOCALAPPDATA%\Famo，绝不碰 %AppData%\Rime（形态甲）。

  注意：实际"部署"(FamoDeploy /deploy 重编译 prism/词库)需已构建的 deployer，
  本机无 C++ 工具链，故 deploy 列为 EXECUTION-PLAN S3.4 的 Manual follow-up。
  本脚本只做 seed（文件拷贝），可独立验证。

.PARAMETER Force
  目标已存在引擎配置时也覆盖（默认跳过已存在的非 famo-settings.json 文件，幂等保守）。
#>
param([switch] $Force)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$payload = Resolve-Path (Join-Path $here '..\famo-config\payload')
$dest = Join-Path $env:LOCALAPPDATA 'Famo'

if (-not (Test-Path $payload)) {
  throw "payload 不存在：$payload —— 先在 famo-config 运行 assemble-payload.sh"
}
New-Item -ItemType Directory -Force -Path $dest | Out-Null

$srcFiles = Get-ChildItem $payload -Recurse -File
$copied = 0; $skipped = 0
foreach ($f in $srcFiles) {
  $rel = $f.FullName.Substring($payload.Path.Length).TrimStart('\')
  $target = Join-Path $dest $rel
  $td = Split-Path $target
  if (-not (Test-Path $td)) { New-Item -ItemType Directory -Force -Path $td | Out-Null }
  if ((Test-Path $target) -and -not $Force) { $skipped++; continue }
  Copy-Item $f.FullName $target -Force
  $copied++
}

Write-Output "seed -> $dest"
Write-Output "  payload files: $($srcFiles.Count) | copied: $copied | skipped(existing): $skipped"
Write-Output "  preserved settings store: $(if (Test-Path (Join-Path $dest 'famo-settings.json')) {'famo-settings.json present'} else {'(none yet — WinUI app seeds it)'})"
# 出厂默认锚点抽查（来自法墨 overlay 的 *.custom.yaml）。
$ice = Join-Path $dest 'rime_ice.custom.yaml'
if (Test-Path $ice) {
  Write-Output "  rime_ice.custom.yaml present (emoji-off 锚点见该文件 switches；tone_display 已下线)"
}
