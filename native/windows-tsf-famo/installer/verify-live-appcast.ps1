<#
.SYNOPSIS
  Verify the public stable WinSparkle feed and its real installer asset.
.DESCRIPTION
  This check is intentionally secret-free. It downloads the live appcast and
  installer, validates the pinned release metadata, and verifies the installer
  with the EdDSA public key embedded in the shipped client.
#>
[CmdletBinding()]
param(
  [ValidatePattern('^\d+\.\d+\.\d+$')]
  [string] $ExpectedVersion = '',
  [ValidatePattern('^https://')]
  [string] $FeedUrl =
    'https://github.com/semantic-craft/famotype-win/releases/latest/download/appcast.xml',
  [string] $WinSparkleToolPath = '',
  [ValidateSet('', '/SILENT /SP- /NOICONS')]
  [string] $RequiredInstallerArguments = ''
)

$ErrorActionPreference = 'Stop'
$ExpectedPublicKey = 'gmOZRp5x2eKXmRczTPlX7hMtVZStjSXJFgovIAw5HdM='
$AllowedInstallerArguments = @(
  '/SILENT /SP- /NOICONS',
  '/SILENT /SP- /NOICONS /NORESTART'
)
$ExpectedMinimumSystemVersion = '10.0.17763'
$SparkleNamespace = 'http://www.andymatuschak.org/xml-namespaces/sparkle'

if (-not $WinSparkleToolPath) {
  $toolCandidates = @(
    $(if ($env:USERPROFILE) {
      Join-Path $env:USERPROFILE '.nuget\packages\winsparkle\0.9.3\tools\winsparkle-tool.exe'
    }),
    (Get-Command winsparkle-tool.exe -ErrorAction SilentlyContinue).Source
  ) | Where-Object {
    $_ -and (Test-Path -LiteralPath $_ -PathType Leaf)
  } | Select-Object -Unique
  $WinSparkleToolPath = $toolCandidates | Select-Object -First 1
}
if (-not $WinSparkleToolPath -or
    -not (Test-Path -LiteralPath $WinSparkleToolPath -PathType Leaf)) {
  throw '未找到 WinSparkle 0.9.3 验签工具；先还原 FamoSettings.csproj。'
}

function Invoke-WithRetry(
  [scriptblock] $Action,
  [string] $Description
) {
  $delays = @(0, 2, 4, 8)
  for ($attempt = 0; $attempt -lt $delays.Count; $attempt++) {
    if ($delays[$attempt] -gt 0) {
      Start-Sleep -Seconds $delays[$attempt]
    }
    try {
      & $Action
      return
    }
    catch {
      if ($attempt -eq $delays.Count - 1) {
        throw
      }
      Write-Warning "$Description 失败，将重试：$($_.Exception.Message)"
    }
  }
}

function Invoke-WinSparkleTool([string[]] $Arguments) {
  $nativeArgs = @('/d', '/c', 'call', $WinSparkleToolPath) + $Arguments
  $output = & $env:ComSpec @nativeArgs 2>&1
  $exitCode = $LASTEXITCODE
  if ($null -eq $exitCode) {
    throw '当前宿主未等待 Windows 原生命令；请从 WSL 通过 cmd.exe /d /c "pwsh ..." 运行。'
  }
  if ($exitCode -ne 0) {
    throw "WinSparkle 验签失败（exit=$exitCode）：$($output -join [Environment]::NewLine)"
  }
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) (
  'famo-live-appcast-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null

try {
  $appcastPath = Join-Path $testRoot 'appcast.xml'
  Invoke-WithRetry -Description '下载稳定 appcast' -Action {
    Remove-Item -LiteralPath $appcastPath -Force -ErrorAction SilentlyContinue
    $requestParameters = @{
      Uri = $FeedUrl
      MaximumRedirection = 5
      UseBasicParsing = $true
      OutFile = $appcastPath
    }
    $null = Invoke-WebRequest @requestParameters
  }

  $readerSettings = [Xml.XmlReaderSettings]::new()
  $readerSettings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
  $readerSettings.XmlResolver = $null
  $reader = [Xml.XmlReader]::Create($appcastPath, $readerSettings)
  try {
    $feed = [Xml.XmlDocument]::new()
    $feed.XmlResolver = $null
    $feed.Load($reader)
  }
  finally {
    $reader.Dispose()
  }

  $namespaceManager = [Xml.XmlNamespaceManager]::new($feed.NameTable)
  $namespaceManager.AddNamespace('sparkle', $SparkleNamespace)
  $items = $feed.SelectNodes('/rss/channel/item')
  if ($items.Count -ne 1) {
    throw "稳定 appcast 必须且只能包含一个版本，实际为 $($items.Count)。"
  }
  $item = $items.Item(0)
  $versionNode = $item.SelectSingleNode('sparkle:version', $namespaceManager)
  $releaseNotesNode = $item.SelectSingleNode(
    'sparkle:releaseNotesLink', $namespaceManager)
  $minimumSystemNode = $item.SelectSingleNode(
    'sparkle:minimumSystemVersion', $namespaceManager)
  $enclosure = $item.SelectSingleNode('enclosure')
  if (-not $versionNode -or -not $releaseNotesNode -or
      -not $minimumSystemNode -or -not $enclosure) {
    throw '稳定 appcast 缺少版本、发行说明、最低系统版本或安装包。'
  }

  $version = $versionNode.InnerText.Trim()
  if ($version -notmatch '^\d+\.\d+\.\d+$') {
    throw "appcast 版本不是三段版本号：$version"
  }
  if ($ExpectedVersion -and $version -cne $ExpectedVersion) {
    throw "稳定 appcast 版本不匹配：expected=$ExpectedVersion actual=$version"
  }

  $expectedInstallerName = "Famo-Setup-$version.exe"
  $expectedInstallerUrl =
    "https://github.com/semantic-craft/famotype-win/releases/download/$version/$expectedInstallerName"
  $expectedReleaseNotesUrl =
    "https://github.com/semantic-craft/famotype-win/releases/tag/$version"
  $downloadUrl = $enclosure.GetAttribute('url')
  if ($downloadUrl -cne $expectedInstallerUrl) {
    throw "安装包 URL 不是同版本不可变资产：$downloadUrl"
  }
  if ($releaseNotesNode.InnerText.Trim() -cne $expectedReleaseNotesUrl) {
    throw '发行说明 URL 与稳定版本不匹配。'
  }
  if ($minimumSystemNode.InnerText.Trim() -cne $ExpectedMinimumSystemVersion) {
    throw '最低 Windows 版本与客户端发布契约不匹配。'
  }
  if ($enclosure.GetAttribute('os', $SparkleNamespace) -cne 'windows-x64') {
    throw 'appcast 缺少 sparkle:os="windows-x64" 平台约束。'
  }
  $installerArguments =
    $enclosure.GetAttribute('installerArguments', $SparkleNamespace)
  if ($RequiredInstallerArguments -and
      $installerArguments -cne $RequiredInstallerArguments) {
    throw "新稳定版安装参数不匹配：$installerArguments"
  }
  if ($AllowedInstallerArguments -cnotcontains $installerArguments) {
    throw 'sparkle:installerArguments 必须是已批准的可见进度安装参数。'
  }

  $signature = $enclosure.GetAttribute('edSignature', $SparkleNamespace)
  if ($signature -notmatch '^[A-Za-z0-9+/]+={0,2}$') {
    throw 'appcast 缺少合法的 sparkle:edSignature。'
  }
  try {
    $signatureBytes = [Convert]::FromBase64String($signature)
  }
  catch {
    throw 'appcast 的 EdDSA 签名不是合法 Base64。'
  }
  if ($signatureBytes.Length -ne 64) {
    throw "appcast 的 Ed25519 签名长度错误：$($signatureBytes.Length)"
  }

  [long] $declaredLength = 0
  $lengthText = $enclosure.GetAttribute('length')
  if (-not [long]::TryParse(
      $lengthText,
      [Globalization.NumberStyles]::None,
      [Globalization.CultureInfo]::InvariantCulture,
      [ref] $declaredLength) -or $declaredLength -le 0) {
    throw "appcast 安装包长度无效：$lengthText"
  }

  $installerPath = Join-Path $testRoot $expectedInstallerName
  Invoke-WithRetry -Description '下载稳定安装包' -Action {
    Remove-Item -LiteralPath $installerPath -Force -ErrorAction SilentlyContinue
    $requestParameters = @{
      Uri = $downloadUrl
      MaximumRedirection = 5
      UseBasicParsing = $true
      OutFile = $installerPath
    }
    $null = Invoke-WebRequest @requestParameters
  }
  $installer = Get-Item -LiteralPath $installerPath -ErrorAction Stop
  if ($installer.Length -ne $declaredLength) {
    throw "安装包长度不匹配：expected=$declaredLength actual=$($installer.Length)"
  }

  Invoke-WinSparkleTool -Arguments @(
    'verify',
    '--public-key', $ExpectedPublicKey,
    '--signature', $signature,
    $installer.FullName
  )

  $success = "PASS: stable appcast $version; $declaredLength bytes; EdDSA verified"
  Write-Host $success -ForegroundColor Green
}
finally {
  if (Test-Path -LiteralPath $testRoot -PathType Container) {
    Remove-Item -LiteralPath $testRoot -Recurse -Force
  }
}
