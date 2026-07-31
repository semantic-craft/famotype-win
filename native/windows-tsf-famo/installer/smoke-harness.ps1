[CmdletBinding()]
param(
  [string] $Installer = '',
  [switch] $RequireRuntimeOnly,
  [switch] $DryRun
)

# Focused local smoke only. This script does not provision VMs, download files,
# install extra applications, create evidence bundles, reboot, or uninstall.
$ErrorActionPreference = 'Stop'
$InstallerDir = $PSScriptRoot
$NativeDir = Split-Path -Parent $InstallerDir
$HealthScript = Join-Path $NativeDir 'weasel-fork\tests\Test-FamoHealth.ps1'
$RegistrationScript = Join-Path $NativeDir 'weasel-fork\tests\Test-FamoTsfRegistration.ps1'

function Need([string] $Path, [string] $Hint) {
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    throw "缺失：$Path`n$Hint"
  }
}

function Get-BridgeSnapshot {
  $brandKey = 'HKLM:\Software\Famo\InputMethod'
  if (-not (Test-Path -LiteralPath $brandKey)) {
    throw 'RequireRuntimeOnly 需要一份已完成 stable Bridge 迁移的 Ready 安装。'
  }
  $brand = Get-ItemProperty -LiteralPath $brandKey
  if ([string]$brand.InstallState -eq 'PendingReboot') {
    throw '当前安装仍处于 PendingReboot，不能作为 runtime-only 基线。'
  }
  if ([string]$brand.InstallState -ne 'Ready') {
    throw "当前安装状态不是 Ready：$($brand.InstallState)"
  }
  $bridge = [string]$brand.BridgePath
  $bridgeHash = [string]$brand.BridgeHash
  $bridgeAbi = [string]$brand.BridgeAbi
  Need $bridge 'Ready 投影缺少 stable Bridge 文件。'
  $item = Get-Item -LiteralPath $bridge
  $actualHash = (Get-FileHash -LiteralPath $bridge -Algorithm SHA256).Hash
  if ($bridgeHash -cne $actualHash) {
    throw "Ready BridgeHash 与文件不一致：registry=$bridgeHash file=$actualHash"
  }
  $fileIdOutput = & fsutil.exe file queryfileid $bridge 2>&1
  $fileIdExit = $LASTEXITCODE
  if ($fileIdExit -ne 0) {
    throw "无法读取 stable Bridge NTFS FileId：$($fileIdOutput -join ' ')"
  }
  [pscustomobject]@{
    BridgePath = [IO.Path]::GetFullPath($bridge)
    BridgeHash = $bridgeHash
    BridgeAbi = $bridgeAbi
    FileId = ($fileIdOutput -join ' ').Trim()
    Length = $item.Length
    CreationTimeUtc = $item.CreationTimeUtc.Ticks
    LastWriteTimeUtc = $item.LastWriteTimeUtc.Ticks
    RuntimeTarget = [IO.Path]::GetFullPath([string]$brand.InstallDir)
  }
}

if (-not $Installer) {
  $Installer = Get-ChildItem -LiteralPath (Join-Path $InstallerDir 'dist') `
    -Filter 'Famo-Setup-*.exe' -File -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1 -ExpandProperty FullName
}

Need $Installer '先构建一个本地安装器，或用 -Installer 指定现有产物。'
Need $HealthScript '缺少本地健康检查脚本。'
Need $RegistrationScript '缺少本地 TSF 注册检查脚本。'

Write-Host '== Famo current-machine smoke ==' -ForegroundColor Cyan
Write-Host "Installer: $Installer"
Write-Host "SHA256: $((Get-FileHash -LiteralPath $Installer -Algorithm SHA256).Hash)"

if ($DryRun) {
  Write-Host 'DRY RUN PASS: local inputs exist; no machine state changed.' -ForegroundColor Green
  exit 0
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw '请只打开一次管理员 PowerShell，再在同一窗口运行本脚本；脚本不会自行重复触发 UAC。'
}

$bridgeBefore = if ($RequireRuntimeOnly) { Get-BridgeSnapshot } else { $null }
$arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-')
$install = Start-Process -FilePath $Installer -ArgumentList $arguments -PassThru
$install.WaitForExit()
if ($install.ExitCode -ne 0) {
  throw "本地安装/修复失败，退出码 $($install.ExitCode)。"
}

if ($RequireRuntimeOnly) {
  $bridgeAfter = Get-BridgeSnapshot
  foreach ($property in @(
    'BridgePath',
    'BridgeHash',
    'BridgeAbi',
    'FileId',
    'Length',
    'CreationTimeUtc',
    'LastWriteTimeUtc')) {
    if ($bridgeBefore.$property -cne $bridgeAfter.$property) {
      throw "runtime-only 升级触碰了 stable Bridge：$property before=$($bridgeBefore.$property) after=$($bridgeAfter.$property)"
    }
  }
  if ($bridgeBefore.RuntimeTarget -eq $bridgeAfter.RuntimeTarget) {
    throw 'runtime-only smoke 没有形成新的版本化 Runtime target。'
  }
  Write-Host 'RUNTIME-ONLY SMOKE PASS: Bridge path/hash/FileId/timestamps unchanged; state=Ready.' -ForegroundColor Green
}

$shell = (Get-Command pwsh.exe -ErrorAction SilentlyContinue).Source
if (-not $shell) {
  $shell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
}

foreach ($probe in @($HealthScript, $RegistrationScript)) {
  & $shell -NoProfile -ExecutionPolicy Bypass -File $probe
  $probeExit = $LASTEXITCODE
  if ($probeExit -ne 0) {
    throw "本地检查失败：$probe（退出码 $probeExit）。"
  }
}

Write-Host 'LOCAL SMOKE PASS' -ForegroundColor Green
Write-Host '下一步只需在当前电脑复现原来的 Explorer 长输入场景；不要扩成 VM 或应用矩阵。'
