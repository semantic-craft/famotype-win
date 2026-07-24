[CmdletBinding()]
param(
  [string] $Installer = '',
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

$arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-')
$install = Start-Process -FilePath $Installer -ArgumentList $arguments -Wait -PassThru
if ($install.ExitCode -ne 0) {
  throw "本地安装/修复失败，退出码 $($install.ExitCode)。"
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
