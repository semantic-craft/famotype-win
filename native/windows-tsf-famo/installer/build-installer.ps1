<#
.SYNOPSIS
  Build the transactional Famo Windows installer from one stable native output.
.NOTES
  Stable packaging rejects unsigned product binaries by default. Use
  -AllowUnsignedDevelopment only for local install/repair/uninstall validation.
#>
param(
  [string] $NativeOutput = '',
  [ValidatePattern('^\d+\.\d+\.\d+$')]
  [string] $AppVersion = '1.4.8',
  [ValidateSet('Stable')]
  [string] $Identity = 'Stable',
  [string] $Configuration = 'Release',
  [switch] $AllowUnsignedDevelopment
)

$ErrorActionPreference = 'Stop'
$InstallerDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$NativeDir = Split-Path -Parent $InstallerDir
if (-not $NativeOutput) {
  $NativeOutput = Join-Path $NativeDir 'text-service\build-msvc-installer-stable'
}
$PayloadDir = Join-Path $NativeDir 'famo-config\payload'
$OverlayDir = Join-Path $NativeDir 'famo-config\overlay'
$SettingsProj = Join-Path $NativeDir 'settings-winui\FamoSettings\FamoSettings.csproj'
$Staging = Join-Path $InstallerDir 'staging'
$PayloadStage = Join-Path $Staging 'payload'
$Iss = Join-Path $InstallerDir 'famo-setup.iss'
$Sbom = Join-Path $InstallerDir 'SBOM.spdx.json'

function Need([string] $Path, [string] $Hint) {
  if (-not (Test-Path -LiteralPath $Path)) { throw "缺失：$Path`n$Hint" }
}

function NeedSameFileHash([string] $Expected, [string] $Actual, [string] $Hint) {
  Need $Expected $Hint
  Need $Actual $Hint
  if ((Get-FileHash -LiteralPath $Expected -Algorithm SHA256).Hash -ne
      (Get-FileHash -LiteralPath $Actual -Algorithm SHA256).Hash) {
    throw "文件不同步：$Actual`n应与：$Expected`n$Hint"
  }
}

function Find-DotNetSdk {
  $candidates = @(
    $(if ($env:DOTNET_ROOT) { Join-Path $env:DOTNET_ROOT 'dotnet.exe' }),
    (Get-Command dotnet.exe -ErrorAction SilentlyContinue).Source
  ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique
  foreach ($candidate in $candidates) {
    $sdks = & $candidate --list-sdks 2>$null
    if ($LASTEXITCODE -eq 0 -and $sdks) { return $candidate }
  }
  throw '未找到包含 SDK 的 dotnet.exe。'
}

function Test-PayloadManifest([string] $Root, [string] $ManifestPath) {
  $lines = [IO.File]::ReadAllLines($ManifestPath)
  $declaredCount = [int](($lines | Where-Object { $_ -like 'file_count=*' }) -replace '^file_count=', '')
  $entries = @($lines | Where-Object { $_ -like 'file=*' })
  if ($declaredCount -ne $entries.Count) { throw 'payload manifest file_count 不一致。' }

  $actual = @(Get-ChildItem -LiteralPath $Root -Recurse -File |
    Where-Object { $_.FullName -ne $ManifestPath })
  if ($actual.Count -ne $declaredCount) { throw 'payload manifest 未覆盖全部文件。' }

  foreach ($entry in $entries) {
    $parts = $entry.Substring(5).Split('|')
    if ($parts.Count -ne 3 -or $parts[0].Contains('..')) { throw "非法 manifest 行：$entry" }
    $file = Join-Path $Root $parts[0]
    Need $file 'payload manifest 引用了不存在的文件。'
    $item = Get-Item -LiteralPath $file
    if ($item.Length -ne [long]$parts[1]) { throw "payload 大小不匹配：$($parts[0])" }
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $parts[2]) {
      throw "payload SHA256 不匹配：$($parts[0])"
    }
  }
}

Write-Host '== 1) Validate one stable native output ==' -ForegroundColor Cyan
Need $NativeOutput '先用 -DFAMO_IDENTITY=Stable 构建 text-service。'
$cache = Join-Path $NativeOutput 'CMakeCache.txt'
Need $cache 'native output 缺少 CMakeCache.txt，无法证明 identity。'
if (-not (Select-String -LiteralPath $cache -Pattern '^FAMO_IDENTITY:STRING=Stable$' -Quiet)) {
  throw 'native output 不是 FAMO_IDENTITY=Stable；禁止把开发 GUID/endpoint 装成正式身份。'
}
$configurationOutput = Join-Path $NativeOutput $Configuration
if (Test-Path -LiteralPath $configurationOutput) {
  $NativeOutput = $configurationOutput
}
$nativeFiles = @('FamoTextService.dll', 'FamoRuntime.exe', 'FamoRimeEngine.dll', 'FamoProfileTool.exe', 'rime.dll')
foreach ($name in $nativeFiles) { Need (Join-Path $NativeOutput $name) 'stable native output 不完整。' }

$unsigned = @($nativeFiles | Where-Object {
  (Get-AuthenticodeSignature -LiteralPath (Join-Path $NativeOutput $_)).Status -ne 'Valid'
})
if ($unsigned.Count -gt 0 -and -not $AllowUnsignedDevelopment) {
  throw "正式安装器拒绝未签名产物：$($unsigned -join ', ')"
}
if ($unsigned.Count -gt 0) {
  Write-Warning "development-only unsigned payload: $($unsigned -join ', ')"
}

Need $PayloadDir '先生成 famo-config/payload。'
foreach ($required in @('default.yaml', 'weasel.yaml', 'opencc')) {
  Need (Join-Path $PayloadDir $required) 'payload 必须自足，不再从 Weasel build 回填。'
}
foreach ($icon in @('famo_zh.ico', 'famo_ascii.ico')) {
  NeedSameFileHash (Join-Path $OverlayDir $icon) (Join-Path $PayloadDir $icon) 'payload 图标过期。'
}

Write-Host '== 2) Create clean final-layout staging ==' -ForegroundColor Cyan
$installerFull = [IO.Path]::GetFullPath($InstallerDir).TrimEnd('\') + '\'
$stagingFull = [IO.Path]::GetFullPath($Staging)
if (-not $stagingFull.StartsWith($installerFull, [StringComparison]::OrdinalIgnoreCase)) {
  throw "拒绝清理 installer 之外的 staging：$stagingFull"
}
if (Test-Path -LiteralPath $Staging) { Remove-Item -LiteralPath $Staging -Recurse -Force }
foreach ($dir in @($PayloadStage, (Join-Path $PayloadStage 'data'), (Join-Path $PayloadStage 'settings'), (Join-Path $PayloadStage 'licenses'))) {
  New-Item -ItemType Directory -Path $dir -Force | Out-Null
}
foreach ($name in $nativeFiles) { Copy-Item -LiteralPath (Join-Path $NativeOutput $name) -Destination $PayloadStage -Force }
Copy-Item -Path (Join-Path $PayloadDir '*') -Destination (Join-Path $PayloadStage 'data') -Recurse -Force
Copy-Item -LiteralPath (Join-Path $InstallerDir 'start_service.bat') -Destination $PayloadStage -Force

Write-Host '== 3) Publish settings ==' -ForegroundColor Cyan
Need $SettingsProj '设置面板工程缺失。'
$dotnet = Find-DotNetSdk
& $dotnet publish $SettingsProj -c $Configuration -r win-x64 --self-contained `
  -p:WindowsAppSDKSelfContained=true -p:WindowsPackageType=None `
  -o (Join-Path $PayloadStage 'settings') --nologo
if ($LASTEXITCODE -ne 0) { throw 'dotnet publish 失败。' }

[xml]$project = Get-Content -LiteralPath $SettingsProj
$targetFramework = @($project.Project.PropertyGroup | ForEach-Object { $_.TargetFramework } | Where-Object { $_ })[0]
$resourceDir = Join-Path (Split-Path -Parent $SettingsProj) "bin\$Configuration\$targetFramework\win-x64"
$settingsStage = Join-Path $PayloadStage 'settings'
foreach ($name in @('FamoSettings.pri', 'App.xbf', 'MainWindow.xbf')) {
  Need (Join-Path $resourceDir $name) "WinUI publish 缺少 $name。"
  Copy-Item -LiteralPath (Join-Path $resourceDir $name) -Destination $settingsStage -Force
}
Need (Join-Path $resourceDir 'Theming') 'WinUI publish 缺少 Theming 资源。'
New-Item -ItemType Directory -Path (Join-Path $settingsStage 'Theming') -Force | Out-Null
Copy-Item -Path (Join-Path $resourceDir 'Theming\*.xbf') -Destination (Join-Path $settingsStage 'Theming') -Force

Copy-Item -LiteralPath (Join-Path $InstallerDir 'LICENSE') -Destination (Join-Path $PayloadStage 'licenses\LICENSE') -Force
Copy-Item -LiteralPath (Join-Path $InstallerDir 'THIRD-PARTY-NOTICES.txt') -Destination (Join-Path $PayloadStage 'licenses\THIRD-PARTY-NOTICES.txt') -Force
Need $Sbom 'SPDX SBOM 缺失。'
Copy-Item -LiteralPath $Sbom -Destination (Join-Path $PayloadStage 'licenses\SBOM.spdx.json') -Force

Write-Host '== 4) Emit and self-check payload manifest ==' -ForegroundColor Cyan
$manifestPath = Join-Path $PayloadStage 'payload-manifest.txt'
$files = @(Get-ChildItem -LiteralPath $PayloadStage -Recurse -File | Sort-Object FullName)
$lines = [Collections.Generic.List[string]]::new()
foreach ($line in @('format=1', 'product=Famo', "version=$AppVersion", 'protocol=1', 'architecture=x64', 'identity=Stable', "file_count=$($files.Count)")) {
  $lines.Add($line)
}
foreach ($file in $files) {
  $relative = $file.FullName.Substring($PayloadStage.Length + 1)
  $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
  $lines.Add("file=$relative|$($file.Length)|$hash")
}
[IO.File]::WriteAllLines($manifestPath, $lines, [Text.UTF8Encoding]::new($false))
Test-PayloadManifest $PayloadStage $manifestPath
$manifestHash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash
$manifestPrefix = $manifestHash.Substring(0, 12)

Write-Host '== 5) Compile installer ==' -ForegroundColor Cyan
$iscc = @(
  (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source,
  "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
  "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
if (-not $iscc) { throw '未找到 ISCC.exe。' }
& $iscc "/DAppVersion=$AppVersion" "/DManifestPrefix=$manifestPrefix" "/DIdentity=$Identity" $Iss
if ($LASTEXITCODE -ne 0) { throw 'ISCC 编译失败。' }

$exe = Get-ChildItem -LiteralPath (Join-Path $InstallerDir 'dist') -Filter "Famo-Setup-$AppVersion.exe" | Select-Object -First 1
Need $exe.FullName 'ISCC 未生成预期安装器。'
Write-Host "OK -> $($exe.FullName)" -ForegroundColor Green
Write-Host "SHA256 = $((Get-FileHash -LiteralPath $exe.FullName -Algorithm SHA256).Hash)"
