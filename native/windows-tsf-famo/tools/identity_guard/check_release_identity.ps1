#requires -version 5
<#
.SYNOPSIS
  Fail a Famo release artifact tree when legacy Weasel identity leaks outside allowed paths.

.DESCRIPTION
  This is the first M0 identity guard. It scans file/directory names and common text files for
  Weasel / 小狼毫. Legal notices and explicit legacy adapter paths are allowed because provenance is
  still required.

.PARAMETER Path
  Artifact root to scan. Defaults to native/windows-tsf-famo/installer/staging.

.PARAMETER SelfTest
  Runs a small temp-tree self-check and exits.
#>
[CmdletBinding()]
param(
  [string] $Path,
  [switch] $SelfTest
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$Script:XiaoLangHao = ([string][char]0x5C0F) + ([string][char]0x72FC) + ([string][char]0x6BEB)
$Script:ForbiddenPatterns = @(
  [pscustomobject]@{ Label = 'Weasel'; Pattern = '(?i)weasel' },
  [pscustomobject]@{ Label = $Script:XiaoLangHao; Pattern = [regex]::Escape($Script:XiaoLangHao) }
)

$Script:TextExtensions = @(
  '.bat', '.cmd', '.config', '.css', '.htm', '.html', '.ini', '.iss', '.json',
  '.log', '.md', '.ps1', '.txt', '.xml', '.yaml', '.yml'
)

# PE 二进制（VersionInfo 等资源）品牌扫描：text-grep 扫不到 PE 资源，曾漏放小狼毫/式恕堂
# 进发布二进制的 VersionInfo（属性页/UAC 可见）。此处扫 .dll/.exe，但**只扫纯产品品牌串**
# —— 不扫 'weasel'：内部 C++ 类名/符号（WeaselTSF 等）是允许保留的实现细节（指南 §3.3），
# 扫它会误报；小狼毫/式恕堂（式恕堂=上游作者名）只可能出现在品牌资源里，无合法内部用途。
$Script:BinaryExtensions = @('.dll', '.exe')
# 捆绑的第三方预编译引擎/库二进制：按各自许可证原样分发（provenance 保留在 THIRD-PARTY-NOTICES），
# 非法墨自建，其内部 upstream 品牌串（如 librime rime.dll 的式恕堂）不在本 guard 责任范围 —— 改写
# 第三方二进制资源既无意义也不合适。只扫法墨自建/发布改名的二进制（FamoTsf/FamoRuntime/FamoDeploy）。
$Script:ThirdPartyBinaries = @('rime.dll', 'winsparkle.dll')
$Script:ShiShuTang = ([string][char]0x5F0F) + ([string][char]0x6055) + ([string][char]0x5802)
$Script:BinaryBrandPatterns = @($Script:XiaoLangHao, $Script:ShiShuTang)

function Get-DefaultArtifactRoot {
  $nativeRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
  return (Join-Path $nativeRoot 'installer\staging')
}

function Get-RelativePath {
  param([string] $Root, [string] $FullName)

  $rootFull = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
  $full = [System.IO.Path]::GetFullPath($FullName)
  if ($full.StartsWith($rootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
    return $full.Substring($rootFull.Length)
  }
  return $full
}

function Test-AllowedIdentityPath {
  param([string] $RelativePath)

  $p = ($RelativePath -replace '\\', '/')
  if ($p -match '(?i)(^|/)(licenses?|third[-_ ]party[-_ ]notices|notices?)(/|$)') { return $true }
  if ($p -match '(?i)(^|/)third[-_ ]party[-_ ]notices\.(txt|html|md)$') { return $true }
  if ($p -match '(?i)(^|/)(legacy|legacy[-_][^/]+)(/|$)') { return $true }
  return $false
}

function Test-AllowedRuntimeCompatibilityPath {
  param([string] $RelativePath)

  $p = ($RelativePath -replace '\\', '/')
  return ($p -match '(?i)^(data/)?weasel(\.custom)?\.yaml$')
}

function Test-SkipIdentityContentPath {
  param([string] $RelativePath)

  $p = ($RelativePath -replace '\\', '/')
  if ($p -match '(?i)\.dict\.yaml$') { return $true }
  if ($p -match '(?i)^(data/)?(cn_dicts|en_dicts|opencc)/') { return $true }
  return $false
}

function Add-Violation {
  param(
    [System.Collections.Generic.List[object]] $Violations,
    [string] $RelativePath,
    [string] $Location,
    [string] $Token
  )

  $Violations.Add([pscustomobject]@{
    path = $RelativePath
    location = $Location
    token = $Token
  })
}

function Test-FamoReleaseIdentity {
  param([string] $Root)

  $resolvedRoot = Resolve-Path -LiteralPath $Root
  $violations = New-Object System.Collections.Generic.List[object]
  $items = @(Get-ChildItem -LiteralPath $resolvedRoot -Recurse -Force)

  foreach ($item in $items) {
    $relative = Get-RelativePath $resolvedRoot $item.FullName
    if (Test-AllowedIdentityPath $relative) { continue }
    $allowRuntimeCompatibilityPath = Test-AllowedRuntimeCompatibilityPath $relative

    foreach ($forbidden in $Script:ForbiddenPatterns) {
      if (-not $allowRuntimeCompatibilityPath -and $relative -match $forbidden.Pattern) {
        Add-Violation $violations $relative 'path' $forbidden.Label
      }
    }

    if ($item.PSIsContainer) { continue }

    if (($Script:BinaryExtensions -contains $item.Extension.ToLowerInvariant()) -and
        ($Script:ThirdPartyBinaries -notcontains $item.Name.ToLowerInvariant())) {
      $bytes = [System.IO.File]::ReadAllBytes($item.FullName)
      $asUtf16 = [System.Text.Encoding]::Unicode.GetString($bytes)
      $asUtf8 = [System.Text.Encoding]::UTF8.GetString($bytes)
      foreach ($brand in $Script:BinaryBrandPatterns) {
        if ($asUtf16.Contains($brand) -or $asUtf8.Contains($brand)) {
          Add-Violation $violations $relative 'binary' $brand
        }
      }
    }

    if ($Script:TextExtensions -notcontains $item.Extension.ToLowerInvariant()) { continue }
    if (Test-SkipIdentityContentPath $relative) { continue }

    $text = $null
    try {
      $text = Get-Content -LiteralPath $item.FullName -Raw -Encoding UTF8
    } catch {
      continue
    }

    foreach ($forbidden in $Script:ForbiddenPatterns) {
      if ($text -match $forbidden.Pattern) {
        Add-Violation $violations $relative 'content' $forbidden.Label
      }
    }
  }

  return $violations
}

function Invoke-SelfTest {
  $root = Join-Path ([System.IO.Path]::GetTempPath()) ('famo-identity-guard-' + [guid]::NewGuid().ToString('N'))
  try {
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $root 'licenses') | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $root 'legacy_weasel_adapter') | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $root 'data\cn_dicts') | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $root 'data\opencc') | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $root 'engine') | Out-Null

    Set-Content -LiteralPath (Join-Path $root 'FamoSettings.txt') -Encoding UTF8 -Value 'Famo Input Method Settings'
    Set-Content -LiteralPath (Join-Path $root 'licenses\THIRD-PARTY-NOTICES.txt') -Encoding UTF8 -Value ('Derived from Weasel / ' + $Script:XiaoLangHao + ' with license notices.')
    Set-Content -LiteralPath (Join-Path $root 'legacy_weasel_adapter\notes.txt') -Encoding UTF8 -Value 'Weasel compatibility implementation note.'
    Set-Content -LiteralPath (Join-Path $root 'data\weasel.custom.yaml') -Encoding UTF8 -Value 'patch: {}'
    Set-Content -LiteralPath (Join-Path $root 'data\cn_dicts\base.dict.yaml') -Encoding UTF8 -Value ($Script:XiaoLangHao + "`txiao lang hao`t100")
    Set-Content -LiteralPath (Join-Path $root 'data\opencc\others.txt') -Encoding UTF8 -Value ($Script:XiaoLangHao + "`t" + $Script:XiaoLangHao + ' Weasel')
    # 干净二进制：含内部 Weasel 类名(允许保留)、无品牌串 —— 二进制扫描不得误报。
    [System.IO.File]::WriteAllBytes((Join-Path $root 'engine\FamoTsf.dll'),
      [System.Text.Encoding]::Unicode.GetBytes('WeaselTSF internal symbol; FamoNamedPipe'))
    # 第三方捆绑二进制(rime.dll)即便含 upstream 品牌串也应按名单豁免，不得误报。
    [System.IO.File]::WriteAllBytes((Join-Path $root 'engine\rime.dll'),
      [System.Text.Encoding]::Unicode.GetBytes('librime ' + $Script:ShiShuTang))

    $clean = @(Test-FamoReleaseIdentity $root)
    if ($clean.Count -ne 0) {
      throw "clean fixture should pass, got $($clean.Count) violation(s)"
    }

    Set-Content -LiteralPath (Join-Path $root 'data\weasel.yaml') -Encoding UTF8 -Value '# Weasel settings'
    Set-Content -LiteralPath (Join-Path $root 'wizard.txt') -Encoding UTF8 -Value ('Start ' + $Script:XiaoLangHao + ' service via WeaselServer')
    # 脏二进制：PE 资源里泄露品牌 小狼毫(UTF-16) —— 二进制扫描必须抓到(text-grep 抓不到)。
    [System.IO.File]::WriteAllBytes((Join-Path $root 'engine\FamoRuntime.exe'),
      [System.Text.Encoding]::Unicode.GetBytes('ProductName=' + $Script:XiaoLangHao))
    $dirty = @(Test-FamoReleaseIdentity $root)
    if ($dirty.Count -lt 2) {
      throw "dirty fixture should fail on both forbidden tokens, got $($dirty.Count) violation(s)"
    }
    if (-not ($dirty | Where-Object { $_.location -eq 'binary' })) {
      throw "binary brand scan should flag the PE brand leak in engine\FamoRuntime.exe"
    }

    Write-Host 'SELFTEST PASS'
  } finally {
    if (Test-Path -LiteralPath $root) {
      Remove-Item -LiteralPath $root -Recurse -Force
    }
  }
}

if ($SelfTest) {
  Invoke-SelfTest
  exit 0
}

if (-not $Path) { $Path = Get-DefaultArtifactRoot }
if (-not (Test-Path -LiteralPath $Path)) {
  Write-Error "Artifact root not found: $Path"
  exit 2
}

$violations = @(Test-FamoReleaseIdentity $Path)
if ($violations.Count -eq 0) {
  Write-Host "FAMO IDENTITY: PASS ($Path)"
  exit 0
}

Write-Host "FAMO IDENTITY: FAIL ($($violations.Count) violation(s))"
foreach ($v in $violations) {
  Write-Host ('{0} :: {1} :: {2}' -f $v.path, $v.location, $v.token)
}
exit 1
