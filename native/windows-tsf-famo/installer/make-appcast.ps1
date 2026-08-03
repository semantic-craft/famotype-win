<#
.SYNOPSIS
  Sign one immutable Famo installer and emit the WinSparkle appcast asset.
.NOTES
  The EdDSA private key must stay outside the repository. Pass it explicitly or
  set FAMO_UPDATE_PRIVATE_KEY for the release process.
#>
param(
  [ValidatePattern('^\d+\.\d+\.\d+$')]
  [string] $AppVersion = '1.5.19',
  [string] $InstallerPath = '',
  [string] $PrivateKeyPath = $env:FAMO_UPDATE_PRIVATE_KEY,
  [string] $OutputPath = ''
)

$ErrorActionPreference = 'Stop'
$ExpectedPublicKey = 'gmOZRp5x2eKXmRczTPlX7hMtVZStjSXJFgovIAw5HdM='
$InstallerDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $InstallerPath) {
  $InstallerPath = Join-Path $InstallerDir "dist\Famo-Setup-$AppVersion.exe"
}
if (-not $OutputPath) {
  $OutputPath = Join-Path $InstallerDir 'dist\appcast.xml'
}
if ([string]::IsNullOrWhiteSpace($PrivateKeyPath)) {
  throw '缺少 EdDSA 私钥路径：传 -PrivateKeyPath 或设置 FAMO_UPDATE_PRIVATE_KEY。'
}

$installer = Get-Item -LiteralPath $InstallerPath -ErrorAction Stop
$privateKey = Get-Item -LiteralPath $PrivateKeyPath -ErrorAction Stop
$expectedName = "Famo-Setup-$AppVersion.exe"
if ($installer.Name -cne $expectedName) {
  throw "安装器文件名必须与版本严格匹配：$expectedName"
}
if ($installer.Length -le 0) {
  throw '安装器为空，拒绝生成 appcast。'
}

$toolCandidates = @(
  $(if ($env:USERPROFILE) {
    Join-Path $env:USERPROFILE '.nuget\packages\winsparkle\0.9.3\tools\winsparkle-tool.exe'
  }),
  (Get-Command winsparkle-tool.exe -ErrorAction SilentlyContinue).Source
) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } |
  Select-Object -Unique
$tool = $toolCandidates | Select-Object -First 1
if (-not $tool) {
  throw '未找到 WinSparkle 0.9.3 签名工具；先还原 FamoSettings.csproj。'
}

function Invoke-WinSparkleTool([string[]] $Arguments) {
  $nativeArgs = @('/d', '/c', 'call', $tool) + $Arguments
  $output = & $env:ComSpec @nativeArgs 2>&1
  $exitCode = $LASTEXITCODE
  if ($null -eq $exitCode) {
    throw '当前宿主未等待 Windows 原生命令；请从 WSL 通过 cmd.exe /d /c "pwsh ..." 运行。'
  }
  if ($exitCode -ne 0) {
    throw "WinSparkle 工具失败（exit=$exitCode）：$($output -join [Environment]::NewLine)"
  }
  return @($output | ForEach-Object { $_.ToString() } | Where-Object { $_ })
}

$publicKeyArgs = @(
  'public-key',
  '--private-key-file', $privateKey.FullName
)
$publicKeyOutput = Invoke-WinSparkleTool -Arguments $publicKeyArgs
$publicKeyLine = @($publicKeyOutput | Where-Object { $_ -like 'Public key:*' })[0]
$publicKey = $publicKeyLine.Substring('Public key:'.Length).Trim()
if ($publicKey -cne $ExpectedPublicKey) {
  throw 'EdDSA 私钥与客户端内置公钥不匹配，拒绝生成 appcast。'
}

$signArgs = @(
  'sign',
  '--private-key-file', $privateKey.FullName,
  $installer.FullName
)
$signatureOutput = Invoke-WinSparkleTool -Arguments $signArgs
$signature = @($signatureOutput | Where-Object { $_ })[-1].Trim()
if ($signature -notmatch '^[A-Za-z0-9+/]+={0,2}$') {
  throw 'WinSparkle 返回了非法 EdDSA 签名。'
}

$downloadUrl = "https://github.com/semantic-craft/famotype-win/releases/download/$AppVersion/$expectedName"
$releaseNotesUrl = "https://github.com/semantic-craft/famotype-win/releases/tag/$AppVersion"
$published = [DateTimeOffset]::UtcNow.ToString(
  'r', [Globalization.CultureInfo]::InvariantCulture)
$length = $installer.Length
$xml = @"
<?xml version="1.0" encoding="UTF-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>法墨输入法 Windows 更新</title>
    <description>法墨输入法 Windows x64 签名更新频道</description>
    <language>zh-CN</language>
    <item>
      <title>法墨输入法 $AppVersion</title>
      <sparkle:version>$AppVersion</sparkle:version>
      <sparkle:shortVersionString>$AppVersion</sparkle:shortVersionString>
      <sparkle:minimumSystemVersion>10.0.17763</sparkle:minimumSystemVersion>
      <sparkle:releaseNotesLink>$releaseNotesUrl</sparkle:releaseNotesLink>
      <pubDate>$published</pubDate>
      <enclosure url="$downloadUrl"
                 length="$length"
                 type="application/octet-stream"
                 sparkle:os="windows-x64"
                 sparkle:edSignature="$signature"
                 sparkle:installerArguments="/SILENT /SP- /NOICONS" />
    </item>
  </channel>
</rss>
"@

# Parse before writing so malformed release metadata fails closed.
[xml] $validated = $xml
$outputFull = [IO.Path]::GetFullPath($OutputPath)
$outputDir = Split-Path -Parent $outputFull
if (-not (Test-Path -LiteralPath $outputDir -PathType Container)) {
  New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}
[IO.File]::WriteAllText($outputFull, $xml, [Text.UTF8Encoding]::new($false))

Write-Host "OK -> $outputFull" -ForegroundColor Green
Write-Host "SHA256 = $((Get-FileHash -LiteralPath $outputFull -Algorithm SHA256).Hash)"
