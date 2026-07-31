param()

$ErrorActionPreference = 'Stop'
$ExpectedPublicKey = 'gmOZRp5x2eKXmRczTPlX7hMtVZStjSXJFgovIAw5HdM='
$InstallerDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$releaseScript = Join-Path $InstallerDir 'make-appcast.ps1'
$tool = Join-Path $env:USERPROFILE '.nuget\packages\winsparkle\0.9.3\tools\winsparkle-tool.exe'
if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
  throw '未找到 WinSparkle 0.9.3 工具；先还原 FamoSettings.csproj。'
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

$testRoot = Join-Path ([IO.Path]::GetTempPath()) (
  'famo-appcast-selftest-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null

try {
  $version = '9.9.9'
  $installer = Join-Path $testRoot "Famo-Setup-$version.exe"
  $appcast = Join-Path $testRoot 'appcast.xml'
  $wrongPrivateKey = Join-Path $testRoot 'wrong-private.key'
  $wrongAppcast = Join-Path $testRoot 'wrong-appcast.xml'
  [IO.File]::WriteAllBytes($installer, [Text.Encoding]::UTF8.GetBytes('famo updater self-test'))

  $releaseParams = @{
    AppVersion = $version
    InstallerPath = $installer
    OutputPath = $appcast
  }
  & $releaseScript @releaseParams

  [xml] $feed = Get-Content -LiteralPath $appcast -Raw
  $sparkleNamespace = 'http://www.andymatuschak.org/xml-namespaces/sparkle'
  $enclosure = $feed.rss.channel.item.enclosure
  $signature = $enclosure.GetAttribute('edSignature', $sparkleNamespace)
  if ($enclosure.GetAttribute('os', $sparkleNamespace) -ne 'windows-x64') {
    throw 'appcast 缺少 windows-x64 平台约束。'
  }
  if ($enclosure.GetAttribute('installerArguments', $sparkleNamespace) -ne
      '/SILENT /SP- /NOICONS') {
    throw 'appcast 安装参数未保留可见进度或重启提示。'
  }

  $verifyArgs = @(
    'verify',
    '--public-key', $ExpectedPublicKey,
    '--signature', $signature,
    $installer
  )
  $null = Invoke-WinSparkleTool -Arguments $verifyArgs

  $null = Invoke-WinSparkleTool -Arguments @(
    'generate-key',
    '--file', $wrongPrivateKey
  )
  $wrongKeyParams = @{
    AppVersion = $version
    InstallerPath = $installer
    PrivateKeyPath = $wrongPrivateKey
    OutputPath = $wrongAppcast
  }
  try {
    & $releaseScript @wrongKeyParams
    throw 'release script accepted a private key that does not match the client'
  }
  catch {
    if ($_.Exception.Message -notlike '*私钥与客户端内置公钥不匹配*') {
      throw
    }
  }
  if (Test-Path -LiteralPath $wrongAppcast -PathType Leaf) {
    throw 'mismatched private key unexpectedly produced an appcast'
  }

  Write-Host 'PASS: signed immutable appcast verified; mismatched key rejected' -ForegroundColor Green
}
finally {
  if (Test-Path -LiteralPath $testRoot -PathType Container) {
    Remove-Item -LiteralPath $testRoot -Recurse -Force
  }
}
