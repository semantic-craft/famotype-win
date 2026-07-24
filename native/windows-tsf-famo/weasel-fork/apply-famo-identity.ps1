#requires -version 5
<#
.SYNOPSIS
  把上游 Weasel checkout 改写成法墨独立 TSF（S3.2）。

.DESCRIPTION
  形态甲：法墨 = 自带品牌的独立 TSF 输入法，与日用 Weasel 并存、互不干扰。
  本脚本对一个干净的 rime/weasel checkout（pin 见 famo-identity.json）施加法墨 identity：
    1. 覆盖 overlay/ 下的整文件（WeaselTSF/Globals.cpp —— 法墨 GUID）。
    2. 对上游源做精确字符串替换：注册表品牌键 / IPC 窗口·管道·互斥体 / code name / 显示名。
  幂等：重复运行结果一致（替换目标已是法墨值则计 0 处）。

  本机无 MSVC/Boost，无法在此 build（见 EXECUTION-PLAN S3.4 = Manual follow-up）。
  本脚本只做"无 build 的源改写"，可 -DryRun 验证替换计数而不落盘。

.PARAMETER UpstreamDir
  rime/weasel 源码 checkout 路径（递归 submodule）。

.PARAMETER DryRun
  只报告每个文件将发生的替换数，不写文件、不拷 overlay。
#>
param(
  [Parameter(Mandatory = $true)] [string] $UpstreamDir,
  [switch] $DryRun
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
# famo-identity.json is UTF-8 without BOM; Windows PowerShell 5.1 otherwise reads Chinese as ANSI.
$identity = Get-Content (Join-Path $here 'famo-identity.json') -Raw -Encoding UTF8 | ConvertFrom-Json

if (-not (Test-Path $UpstreamDir)) { throw "UpstreamDir 不存在: $UpstreamDir" }

# ── 字符串替换表：相对路径 → @("old`tnew", ...) ─────────────────────────────
# 用 tab 分隔 old/new（这些串都不含 tab），避免 PowerShell 对嵌套数组的展平。
# 注：源里注册表键是 C 转义字面量 'Software\\Rime\\Weasel'（双反斜杠）。字面替换。
$brandOld = 'Software\\Rime\\Weasel'; $brandNew = $identity.registry.brandKey.Replace('\', '\\')
$rimeOld  = 'Software\\Rime';         $rimeNew  = $identity.registry.rimeRootKey.Replace('\', '\\')
# 形态甲硬约束：业务运行时绝不落到 %AppData%\Rime。源里的默认回退（注册表品牌键缺失时）
# 也改写到 %LOCALAPPDATA%\Famo —— 即便品牌键未写，也只会用法墨目录，永不触碰日用 Rime。
$appdataOld = '%AppData%\\Rime';      $appdataNew = $identity.registry.rimeUserDir.Replace('\', '\\')
# 业务二进制发布改名：安装暂存把 WeaselServer.exe→FamoRuntime.exe、WeaselDeployer.exe→FamoDeploy.exe。
# 源里 exec/进程检测/WER 转储的 exe 字面量必须同步改名，否则装后 FamoRuntime 运行时会去 exec 一个
# 不存在的 WeaselDeployer.exe（托盘部署/ensure-deployed/秒切断），TSF 也检测不到已改名的服务进程。
$serverExeOld = 'WeaselServer.exe';   $serverExeNew = $identity.brand.serverExe
$deployExeOld = 'WeaselDeployer.exe'; $deployExeNew = $identity.brand.deployerExe
$legacyWeaselNameZh = ([string][char]0x5C0F) + ([string][char]0x72FC) + ([string][char]0x6BEB)
$T = "`t"

$edits = [ordered]@{
  'include/WeaselConstants.h' = @(
    "$brandOld$T$brandNew",
    "$rimeOld$T$rimeNew",
    "#define WEASEL_CODE_NAME ""Weasel""$T#define WEASEL_CODE_NAME ""Famo"""
  )
  'include/WeaselIPC.h' = @(
    "WeaselIPCWindow_1.0$T$($identity.ipc.ipcWindow)",
    "WeaselNamedPipe$T$($identity.ipc.pipeName)"
  )
  'include/WeaselUtility.h' = @(
    "$brandOld$T$brandNew",
    "return L""$($legacyWeaselNameZh)"";${T}return L""$($identity.brand.displayNameZh)"";",
    "return L""Weasel"";${T}return L""$($identity.brand.displayNameEn)"";"
  )
  'RimeWithWeasel/WeaselUtility.cpp'        = @("$brandOld$T$brandNew", "$appdataOld$T$appdataNew")
  # WeaselDeployerExclusiveMutex（deployer 单实例 + TSF 拉起判定共用）必须 identity 改名，
  # 否则法墨与日用小狼毫共享互斥体（并存承诺破口）。长串条目排在 WeaselDeployerMutex 之前，
  # 防未来有人把两条替换合进同一文件清单时子串先替换破坏长串。
  'WeaselTSF/WeaselTSF.cpp'                 = @("WeaselDeployerExclusiveMutex$T$($identity.ipc.deployerExclusiveMutex)", "$serverExeOld$T$serverExeNew")
  'WeaselDeployer/WeaselDeployer.cpp'       = @("WeaselDeployerExclusiveMutex$T$($identity.ipc.deployerExclusiveMutex)", "$deployExeOld$T$deployExeNew")
  'RimeWithWeasel/RimeWithWeasel.cpp'       = @("WeaselDeployerMutex$T$($identity.ipc.deployerMutex)")
  'WeaselServer/WeaselServerApp.h'          = @("$brandOld$T$brandNew")
  'WeaselServer/WeaselServerApp.cpp'        = @("$brandOld$T$brandNew", "$deployExeOld$T$deployExeNew")
  'WeaselSetup/imesetup.cpp'                = @("$brandOld$T$brandNew", "$serverExeOld$T$serverExeNew")
  'WeaselSetup/WeaselSetup.cpp'             = @("$brandOld$T$brandNew", "$serverExeOld$T$serverExeNew", "$deployExeOld$T$deployExeNew")
  'WeaselDeployer/SwitcherSettingsDialog.cpp' = @("$brandOld$T$brandNew")
  'WeaselDeployer/Configurator.cpp'         = @("WeaselDeployerMutex$T$($identity.ipc.deployerMutex)")
  'WeaselTSF/LanguageBar.cpp'               = @("$brandOld$T$brandNew", "$appdataOld$T$appdataNew")
}

$totalRepl = 0
$missing = @()
Write-Output "=== 法墨 identity 改写  (DryRun=$($DryRun.IsPresent))  upstream=$UpstreamDir ==="

foreach ($rel in $edits.Keys) {
  $path = Join-Path $UpstreamDir $rel
  if (-not (Test-Path $path)) { $missing += $rel; Write-Output ("  [MISS] {0}" -f $rel); continue }
  $text = Get-Content $path -Raw
  $fileRepl = 0
  foreach ($pair in $edits[$rel]) {
    $parts = $pair -split "`t", 2
    $old = $parts[0]; $new = $parts[1]
    $count = ([regex]::Matches($text, [regex]::Escape($old))).Count
    if ($count -gt 0) { $text = $text.Replace($old, $new); $fileRepl += $count }
  }
  $totalRepl += $fileRepl
  if (-not $DryRun -and $fileRepl -gt 0) {
    Set-Content -Path $path -Value $text -NoNewline -Encoding UTF8
  }
  Write-Output ("  [{0,2} repl] {1}" -f $fileRepl, $rel)
}

# ── overlay 整文件覆盖 ───────────────────────────────────────────────────────
$overlayRoot = Join-Path $here 'overlay'
Write-Output "=== overlay 整文件覆盖 ==="
Get-ChildItem $overlayRoot -Recurse -File | ForEach-Object {
  $rel = $_.FullName.Substring($overlayRoot.Length + 1) -replace '\\', '/'
  $dest = Join-Path $UpstreamDir $rel
  if (-not (Test-Path (Split-Path $dest))) { Write-Output ("  [SKIP no target dir] {0}" -f $rel); return }
  if ($DryRun) { Write-Output ("  [would copy] {0}" -f $rel) }
  else { Copy-Item $_.FullName $dest -Force; Write-Output ("  [copied] {0}" -f $rel) }
}

# ── .rc VERSIONINFO 品牌改写（PE 资源里的小狼毫/式恕堂/Weasel/Shishutang → 法墨/Famo）──
# identity guard 只扫文本、不解析 PE，故这些 VersionInfo 串会漏进发布二进制的属性/UAC（P1 泄露）。
# .rc 是 UTF-16LE，须保编码读写（上面的 $edits 走 UTF-8，不可用于 .rc）。逐「带引号完整字面量」
# 精确改写：天然不误伤内部 Weasel* 类名/API（那些没有前后引号包裹的品牌串形态）。
# 注：用 [ordered]@{老=新} 而非 @('老'+$T+'新',...) —— 后者 `+` 会把逗号一起吞掉，
# 整个数组塌缩成 1 个元素（PowerShell 运算符优先级坑）；hashtable 无此问题。
$rcCommon = [ordered]@{
  '"Powered by RIME | 中州韵输入法引擎"'          = '"法墨输入法"'
  '"Powered by RIME | 中州韻輸入法引擎"'          = '"法墨输入法"'
  '"Powered by RIME | Rime Input Method Engine"' = '"Famo Input Method"'
  '"Shishutang"'                = '"Famo"'
  '"Copyleft RIME Developers"'  = '"Famo"'
  '"Weasel"'                    = ('"' + $identity.brand.displayNameEn + '"')
  # 纯中文品牌（决不出现在 .rc 资源标识符/指令里，只在字符串字面量内）用裸串兜底：
  # 一并清掉对话框/字符串表里的残留(如 WeaselDeployer 配置对话框，Famo 已弃用但串仍编进
  # FamoDeploy.exe)，保证发布二进制零小狼毫/式恕堂。放在 per-file 具体 FileDescription 规则之后跑。
  '式恕堂'                      = '法墨'
  '小狼毫'                      = $identity.brand.displayNameZh
}
$rcEdits = [ordered]@{
  'WeaselTSF/WeaselTSF.rc' = [ordered]@{
    '"小狼毫TSF"'      = '"法墨 TSF"'
    '"Weasel TSF"'     = '"Famo TSF"'
    '"WeaselTSF"'      = '"FamoTsf"'
    '"weaselx64.dll"'  = '"FamoTsf.dll"'
  }
  'WeaselServer/WeaselServer.rc' = [ordered]@{
    '"小狼毫算法服务"'   = '"法墨输入服务"'
    '"小狼毫算法服務"'   = '"法墨输入服务"'
    '"Weasel Server"'    = '"Famo Runtime"'
    '"WeaselServer.exe"' = ('"' + $serverExeNew + '"')
    '"WeaselServer"'     = '"FamoRuntime"'
  }
  'WeaselDeployer/WeaselDeployer.rc' = [ordered]@{
    '"小狼毫部署应用"'    = '"法墨部署"'
    '"小狼毫部署應用"'    = '"法墨部署"'
    '"Weasel Deployer"'   = '"Famo Deploy"'
    '"WeaselDeployer"'    = '"FamoDeploy"'
  }
}
$rcTotal = 0
Write-Output "=== .rc VERSIONINFO 品牌改写（UTF-16 保编码）==="
foreach ($rel in $rcEdits.Keys) {
  $path = Join-Path $UpstreamDir $rel
  if (-not (Test-Path $path)) { $missing += $rel; Write-Output ("  [MISS] {0}" -f $rel); continue }
  $text = [System.IO.File]::ReadAllText($path)   # UTF-16LE BOM 自动识别
  $fileRepl = 0
  foreach ($tbl in @($rcEdits[$rel], $rcCommon)) {
    foreach ($old in $tbl.Keys) {
      $count = ([regex]::Matches($text, [regex]::Escape($old))).Count
      if ($count -gt 0) { $text = $text.Replace($old, $tbl[$old]); $fileRepl += $count }
    }
  }
  $rcTotal += $fileRepl
  if (-not $DryRun -and $fileRepl -gt 0) {
    [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UnicodeEncoding($false, $true)))
  }
  Write-Output ("  [{0,2} repl] {1}" -f $fileRepl, $rel)
}

Write-Output "=== 汇总: 源码替换 $totalRepl 处；.rc VersionInfo $rcTotal 处；缺失文件 $($missing.Count) ==="
if ($missing.Count -gt 0) { Write-Output ("缺失: " + ($missing -join ', ')) }
