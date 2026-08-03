<#
.SYNOPSIS
  Build the transactional Famo Windows installer from independent Runtime and
  stable Bridge artifacts.
.NOTES
  Stable packaging rejects unsigned product binaries by default. Use
  -AllowUnsignedDevelopment only for local install/repair/uninstall validation.
#>
param(
  [string] $NativeOutput = '',
  [string] $BridgeArtifact = '',
  [ValidatePattern('^\d+\.\d+\.\d+$')]
  [string] $AppVersion = '1.5.18',
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
if (-not $BridgeArtifact) {
  $BridgeArtifact = Join-Path $NativeDir 'text-service\build-bridge-v5-artifact'
}
$PayloadDir = Join-Path $NativeDir 'famo-config\payload'
$OverlayDir = Join-Path $NativeDir 'famo-config\overlay'
$SettingsProj = Join-Path $NativeDir 'settings-winui\FamoSettings\FamoSettings.csproj'
$Staging = Join-Path $InstallerDir 'staging'
$PayloadStage = Join-Path $Staging 'payload'
$BridgeStage = Join-Path $Staging 'bridge'
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

function Invoke-NativeProcess {
  param(
    [string] $FilePath,
    [string[]] $Arguments
  )

  $nativeArgs = @('/d', '/c', 'call', $FilePath) + $Arguments
  $output = & $env:ComSpec @nativeArgs 2>&1
  $exitCode = $LASTEXITCODE
  if ($null -eq $exitCode) {
    throw '当前宿主未等待 Windows 原生命令；请从 WSL 通过 cmd.exe /d /c "pwsh ..." 运行。'
  }
  [pscustomobject]@{
    ExitCode = $exitCode
    Output = @($output | ForEach-Object { $_.ToString() })
  }
}

function Find-DotNetSdk {
  $candidates = @(
    $(if ($env:DOTNET_ROOT) { Join-Path $env:DOTNET_ROOT 'dotnet.exe' }),
    (Get-Command dotnet.exe -ErrorAction SilentlyContinue).Source
  ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique
  foreach ($candidate in $candidates) {
    $probe = Invoke-NativeProcess -FilePath $candidate -Arguments @('--list-sdks')
    if ($probe.ExitCode -eq 0 -and
        -not [string]::IsNullOrWhiteSpace(($probe.Output -join [Environment]::NewLine))) {
      return $candidate
    }
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

function Read-BridgeArtifact([string] $Root) {
  $manifest = Join-Path $Root 'bridge-manifest.txt'
  $bridge = Join-Path $Root 'FamoTextService.dll'
  Need $manifest '先生成冻结的 stable Bridge artifact。'
  Need $bridge 'Bridge artifact 缺少 FamoTextService.dll。'
  $lines = [IO.File]::ReadAllLines($manifest)
  $values = @{}
  foreach ($line in $lines) {
    if ($line -like 'file=*') { continue }
    $separator = $line.IndexOf('=')
    if ($separator -le 0) { throw "非法 Bridge manifest 行：$line" }
    $key = $line.Substring(0, $separator)
    if ($values.ContainsKey($key)) { throw "重复 Bridge manifest 字段：$key" }
    $values[$key] = $line.Substring($separator + 1)
  }
  foreach ($key in @('format', 'identity', 'bridge_abi', 'protocol_min', 'protocol_max')) {
    if (-not $values.ContainsKey($key)) { throw "Bridge manifest 缺少字段：$key" }
  }
  if ($values.format -ne '1' -or $values.identity -ne 'Stable') {
    throw 'Bridge artifact 格式或 identity 非法。'
  }
  $bridgeAbi = 0
  $protocolMin = 0
  $protocolMax = 0
  if (-not [int]::TryParse($values.bridge_abi, [ref]$bridgeAbi) -or
      -not [int]::TryParse($values.protocol_min, [ref]$protocolMin) -or
      -not [int]::TryParse($values.protocol_max, [ref]$protocolMax) -or
      $bridgeAbi -lt 1 -or $bridgeAbi -gt 65535 -or
      $protocolMin -lt 1 -or $protocolMax -gt 65535 -or
      $protocolMax -lt $protocolMin) {
    throw 'Bridge ABI 或协议范围非法。'
  }
  $entries = @($lines | Where-Object { $_ -like 'file=*' })
  if ($entries.Count -ne 1) { throw 'Bridge manifest 必须且只能声明一个 DLL。' }
  $parts = $entries[0].Substring(5).Split('|')
  if ($parts.Count -ne 3 -or $parts[0] -ne 'FamoTextService.dll') {
    throw 'Bridge manifest 文件项非法。'
  }
  $item = Get-Item -LiteralPath $bridge
  $hash = (Get-FileHash -LiteralPath $bridge -Algorithm SHA256).Hash
  if ($item.Length -ne [long]$parts[1] -or $hash -ne $parts[2] -or
      $item.VersionInfo.FileMajorPart -ne $bridgeAbi) {
    throw 'Bridge artifact 大小、SHA256 或 DLL 文件主版本与 manifest 不一致。'
  }
  [pscustomobject]@{
    Manifest = $manifest
    Dll = $bridge
    Abi = $bridgeAbi
    ProtocolMin = $protocolMin
    ProtocolMax = $protocolMax
    Hash = $hash
  }
}

Write-Host '== 1) Validate Runtime output and stable Bridge artifact ==' -ForegroundColor Cyan
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
$runtimeFiles = @(
  'FamoRuntime.exe',
  'FamoRimeEngine.dll',
  'FamoProfileTool.exe',
  'rime.dll'
)
foreach ($name in $runtimeFiles) { Need (Join-Path $NativeOutput $name) 'stable Runtime output 不完整。' }
$bridgeArtifactInfo = Read-BridgeArtifact $BridgeArtifact
$bridgeAbi = $bridgeArtifactInfo.Abi
$bridgeHash = $bridgeArtifactInfo.Hash

$unsigned = @($runtimeFiles | Where-Object {
  (Get-AuthenticodeSignature -LiteralPath (Join-Path $NativeOutput $_)).Status -ne 'Valid'
})
if ((Get-AuthenticodeSignature -LiteralPath $bridgeArtifactInfo.Dll).Status -ne 'Valid') {
  $unsigned += 'FamoTextService.dll'
}
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
foreach ($required in @('s2t.json', 's2hk.json', 'STCharacters.ocd2',
    'STPhrases.ocd2', 'HKVariants.ocd2', 'OpenCC.LICENSE')) {
  Need (Join-Path $PayloadDir "opencc\$required") 'payload 缺少简繁转换所需的 OpenCC 标准数据。'
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
foreach ($dir in @($PayloadStage, $BridgeStage, (Join-Path $PayloadStage 'data'), (Join-Path $PayloadStage 'settings'), (Join-Path $PayloadStage 'licenses'))) {
  New-Item -ItemType Directory -Path $dir -Force | Out-Null
}
foreach ($name in $runtimeFiles) { Copy-Item -LiteralPath (Join-Path $NativeOutput $name) -Destination $PayloadStage -Force }
Copy-Item -LiteralPath $bridgeArtifactInfo.Dll -Destination $BridgeStage -Force
Copy-Item -LiteralPath $bridgeArtifactInfo.Manifest -Destination $BridgeStage -Force
Copy-Item -Path (Join-Path $PayloadDir '*') -Destination (Join-Path $PayloadStage 'data') -Recurse -Force
Copy-Item -LiteralPath (Join-Path $InstallerDir 'start_service.bat') -Destination $PayloadStage -Force

Write-Host '== 3) Publish settings ==' -ForegroundColor Cyan
Need $SettingsProj '设置面板工程缺失。'
$dotnet = Find-DotNetSdk
$publishArguments = @(
  'publish', $SettingsProj,
  '-c', $Configuration,
  '-r', 'win-x64',
  '--self-contained',
  '-p:WindowsAppSDKSelfContained=true',
  '-p:WindowsPackageType=None',
  "-p:Version=$AppVersion",
  "-p:InformationalVersion=$AppVersion",
  '-o', (Join-Path $PayloadStage 'settings'),
  '--nologo'
)
$publish = Invoke-NativeProcess -FilePath $dotnet -Arguments $publishArguments
$publish.Output | ForEach-Object { Write-Host $_ }
if ($publish.ExitCode -ne 0) { throw 'dotnet publish 失败。' }

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
Need (Join-Path $settingsStage 'WinSparkle.dll') 'WinUI publish 缺少自动更新运行库。'

Copy-Item -LiteralPath (Join-Path $InstallerDir 'LICENSE') -Destination (Join-Path $PayloadStage 'licenses\LICENSE') -Force
Copy-Item -LiteralPath (Join-Path $InstallerDir 'THIRD-PARTY-NOTICES.txt') -Destination (Join-Path $PayloadStage 'licenses\THIRD-PARTY-NOTICES.txt') -Force
Copy-Item -LiteralPath (Join-Path $InstallerDir 'WinSparkle-LICENSE.txt') -Destination (Join-Path $PayloadStage 'licenses\WinSparkle-LICENSE.txt') -Force
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
$isccArguments = @(
  "/DAppVersion=$AppVersion",
  "/DManifestPrefix=$manifestPrefix",
  "/DManifestHash=$manifestHash",
  "/DIdentity=$Identity",
  "/DBridgeAbi=$bridgeAbi",
  "/DBridgeHash=$bridgeHash",
  "/DBridgeProtocolMin=$($bridgeArtifactInfo.ProtocolMin)",
  "/DBridgeProtocolMax=$($bridgeArtifactInfo.ProtocolMax)",
  $Iss
)
$compile = Invoke-NativeProcess -FilePath $iscc -Arguments $isccArguments
$compile.Output | ForEach-Object { Write-Host $_ }
if ($compile.ExitCode -ne 0) { throw 'ISCC 编译失败。' }

$exe = Get-ChildItem -LiteralPath (Join-Path $InstallerDir 'dist') -Filter "Famo-Setup-$AppVersion.exe" | Select-Object -First 1
Need $exe.FullName 'ISCC 未生成预期安装器。'
Write-Host "OK -> $($exe.FullName)" -ForegroundColor Green
Write-Host "SHA256 = $((Get-FileHash -LiteralPath $exe.FullName -Algorithm SHA256).Hash)"
