$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$trayPatch = Get-Content (Join-Path $root 'features/tray-options.patch') -Raw
$languageBarPatch = Get-Content (Join-Path $root 'features/language-bar-menu.patch') -Raw
$statusPatch = Get-Content (Join-Path $root 'features/status-bar.patch') -Raw
$statusBarOverlay = Get-Content (Join-Path $root 'overlay/WeaselUI/FamoStatusBar.cpp') -Raw
$applyScript = Get-Content (Join-Path $root 'apply-famo-statusbar.ps1') -Raw
$menuPatches = $trayPatch + $languageBarPatch + $statusPatch
$overlayResource = Join-Path $root 'overlay/resource'
$configOverlayResource = Resolve-Path (Join-Path $root '../famo-config/overlay')

function Assert-Contains {
  param(
    [string] $Text,
    [string] $Pattern,
    [string] $Message
  )
  if ($Text -notmatch [regex]::Escape($Pattern)) {
    throw $Message
  }
}

function Assert-NotContains {
  param(
    [string] $Text,
    [string] $Pattern,
    [string] $Message
  )
  if ($Text -match [regex]::Escape($Pattern)) {
    throw $Message
  }
}

function Assert-InOrder {
  param(
    [string] $Text,
    [string[]] $Patterns,
    [string] $Message
  )

  $cursor = -1
  foreach ($pattern in $Patterns) {
    $next = $Text.IndexOf($pattern, $cursor + 1, [System.StringComparison]::Ordinal)
    if ($next -lt 0) {
      throw "$Message Missing or out of order: $pattern"
    }
    $cursor = $next
  }
}

function Assert-FamoLanguageIcon {
  param(
    [string] $Path,
    [string] $Name
  )

  if (-not (Test-Path $Path)) {
    throw "Famo overlay must ship a custom $Name language-bar icon"
  }

  $bytes = [System.IO.File]::ReadAllBytes($Path)
  if ($bytes.Length -lt 1024) {
    throw "$Name language-bar icon must contain real image data, not only an ICO directory"
  }
  if ([BitConverter]::ToUInt16($bytes, 0) -ne 0 -or [BitConverter]::ToUInt16($bytes, 2) -ne 1) {
    throw "$Name language-bar icon must be a valid ICO file"
  }

  $count = [BitConverter]::ToUInt16($bytes, 4)
  if ($count -lt 6) {
    throw "$Name language-bar icon must ship multiple taskbar-friendly sizes"
  }

  $sizes = New-Object 'System.Collections.Generic.HashSet[int]'
  for ($i = 0; $i -lt $count; $i++) {
    $base = 6 + 16 * $i
    $w = [int] $bytes[$base]
    if ($w -eq 0) { $w = 256 }
    [void] $sizes.Add($w)
  }
  foreach ($expected in @(16, 24, 32, 48, 64, 256)) {
    if (-not $sizes.Contains($expected)) {
      throw "$Name language-bar icon must include ${expected}px"
    }
  }
}

Assert-Contains $trayPatch 'L"切换至英文输入"' 'tray menu must expose the macOS action label for switching to English'
Assert-Contains $trayPatch 'L"切换至中文输入"' 'tray menu must expose the macOS action label for switching to Chinese'
Assert-Contains $trayPatch 'L"切换至繁体输入"' 'tray menu must expose the macOS action label for switching to Traditional Chinese'
Assert-Contains $trayPatch 'L"切换至简体输入"' 'tray menu must expose the macOS action label for switching to Simplified Chinese'
Assert-Contains $trayPatch 'L"使用英文标点符号"' 'tray menu must expose the macOS action label for English punctuation'
Assert-Contains $trayPatch 'L"使用中文标点符号"' 'tray menu must expose the macOS action label for Chinese punctuation'
Assert-Contains $trayPatch 'L"使用全角输入"' 'tray menu must expose the macOS action label for full shape input'
Assert-Contains $trayPatch 'L"使用半角输入"' 'tray menu must expose the macOS action label for half shape input'

Assert-Contains $trayPatch 'ID_WEASELTRAY_FAMO_STATUS_BAR' 'server command layer must define a real Windows status-bar command'
Assert-Contains $trayPatch 'm_status_bar.ShowOnFocus()' 'Windows status-bar command must show the real floating status bar'
Assert-Contains $trayPatch 'while (::GetMenuItemCount(hMenu) > 0)' 'tray menu must clear the upstream Weasel resource menu before adding Famo items'
Assert-Contains $trayPatch '::DeleteMenu(hMenu, 0, MF_BYPOSITION)' 'tray menu must remove old upstream items such as update/help/dictionary before inserting Famo items'
Assert-NotContains $trayPatch 'if (!m_option_query)' 'tray menu must not fall back to the upstream Weasel menu when option-query injection is missing'
Assert-Contains $trayPatch 'm_option_query ? m_option_query(opt) : false' 'tray menu must still rebuild Famo items with default toggle state when option-query injection is missing'
Assert-Contains $trayPatch 'ID_WEASELTRAY_DEPLOY, L"刷新配置"' 'tray menu must keep a real Windows deploy maintenance command'
Assert-Contains $trayPatch 'ID_WEASELTRAY_QUIT, L"退出"' 'tray menu must keep a real Windows quit command'

Assert-InOrder $trayPatch @(
  'L"切换至英文输入"',
  'L"切换至繁体输入"',
  'L"使用英文标点符号"',
  'L"使用全角输入"',
  'L"设置…"',
  'L"显示悬浮状态条"',
  'L"刷新配置"',
  'L"退出"'
) 'tray menu must preserve the macOS core order before Windows maintenance entries.'

foreach ($removedPatch in @(
  'features/emoji-entry.patch',
  'features/clipboard-entry.patch',
  'features/quick-phrases-entry.patch',
  'features/ai-chat-entry.patch'
)) {
  Assert-NotContains $applyScript $removedPatch "patch chain must not add input-area skill menu patch: $removedPatch"
}
foreach ($removed in @(
  'ID_WEASELTRAY_FAMO_QUICK_PHRASES',
  'ID_WEASELTRAY_FAMO_CLIPBOARD_PANEL',
  'ID_WEASELTRAY_FAMO_EMOJI_PANEL',
  'ID_WEASELTRAY_FAMO_PROMPT_LIBRARY',
  'ID_WEASELTRAY_FAMO_AI_CHAT',
  'ID_WEASELTRAY_FAMO_PROMPT_PICKER',
  'ID_WEASELTRAY_FAMO_PROMPT_SAVE_SELECTION',
  'ID_WEASELTRAY_FAMO_AI_POLISH',
  'ID_WEASELTRAY_FAMO_AI_SOURCE_CHECK',
  'ID_WEASELTRAY_FAMO_AI_RESEARCH',
  'ID_WEASELTRAY_FAMO_AI_DOCUMENT_FORMATTING',
  'launch_famo_settings(dir, L"quick-phrase-picker")',
  'launch_famo_settings(dir, L"clipboard-panel")',
  'launch_famo_settings(dir, L"emoji")',
  'launch_famo_clipboard(dir)',
  'launch_famo_emoji(dir)',
  'launch_famo_settings(dir, L"prompt-library")',
  'launch_famo_settings(dir, L"ai-chat")',
  'launch_famo_settings(dir, L"prompt-picker")',
  'launch_famo_settings(dir, L"prompt-save-selection")',
  'launch_famo_settings(dir, L"ai-polish")',
  'launch_famo_settings(dir, L"ai-source-check")',
  'launch_famo_settings(dir, L"ai-research")',
  'launch_famo_settings(dir, L"ai-document-formatting")',
  'L"快捷短语"',
  'L"快捷短语…"',
  'L"剪贴板历史"',
  'L"剪贴板…"',
  'L"表情符号"',
  'L"表情符号…"',
  'L"提示词库"',
  'L"提示词库…"',
  'L"AI 对话"',
  'L"AI 对话…"',
  'L"快速插入提示词…"',
  'L"保存选中为提示词…"',
  'L"AI 润色选中…"',
  'L"来源核验…"',
  'L"辅助检索…"',
  'L"公文排版…"'
)) {
  Assert-NotContains $menuPatches $removed "right-click/status-bar/language-bar menus must not expose input-area skill entry: $removed"
}
Assert-NotContains $menuPatches '场景词库' 'menus must not show scene vocabulary because Windows scope excludes it'
Assert-NotContains $menuPatches '术语' 'menus must not show terminology/project vocabulary because Windows scope excludes it'
Assert-NotContains $trayPatch 'Check for updates' 'tray menu must keep update checks in the About page instead of the native tray menu'

Assert-Contains $applyScript 'features/language-bar-menu.patch' 'patch chain must also rewrite the TSF language-bar menu path'
Assert-Contains $languageBarPatch 'BuildFamoLanguageBarMenu' 'TSF language-bar menu must be rebuilt at runtime instead of loading the upstream resource menu'
Assert-Contains $languageBarPatch 'CreatePopupMenu()' 'TSF language-bar menu must create a fresh Famo popup menu'
Assert-Contains $languageBarPatch 'GetForegroundWindow()' 'TSF language-bar popup must fall back to the foreground window when TSF has no focused context window'
Assert-Contains $languageBarPatch 'GetDesktopWindow()' 'TSF language-bar popup must have a final owner fallback so the menu can open reliably'
Assert-Contains $languageBarPatch 'SetForegroundWindow(hwnd)' 'TSF language-bar popup must foreground its owner before TrackPopupMenuEx'
Assert-Contains $languageBarPatch 'TPM_RIGHTBUTTON' 'TSF language-bar popup must accept right-button selection semantics'
Assert-Contains $languageBarPatch 'PostMessageW(hwnd, WM_NULL, 0, 0)' 'TSF language-bar popup must post WM_NULL after TrackPopupMenuEx so the menu does not stick'
Assert-Contains $languageBarPatch 'ID_WEASELTRAY_FAMO_STATUS_BAR' 'TSF language-bar menu must expose the Windows-only floating status-bar command'
Assert-Contains $languageBarPatch 'ID_WEASELTRAY_OPT_ASCII_MODE' 'TSF language-bar menu must expose the same real runtime toggle commands as the tray menu'
Assert-Contains $languageBarPatch 'L"设置…"' 'TSF language-bar menu must show the macOS settings label'
Assert-Contains $languageBarPatch 'L"显示悬浮状态条"' 'TSF language-bar menu must show the Windows-only floating status-bar label'
Assert-Contains $languageBarPatch 'full_shape = stat.full_shape' 'TSF language-bar menu must track the known full-shape state for dynamic action labels'
Assert-Contains $languageBarPatch '-        menu = LoadMenuW(g_hInst, MAKEINTRESOURCE(IDR_MENU_POPUP_HANS));' 'TSF language-bar right-click menu must remove the upstream Simplified Chinese resource-menu load'
Assert-Contains $languageBarPatch '-  HMENU menu = LoadMenuW(g_hInst, MAKEINTRESOURCE(IDR_MENU_POPUP));' 'TSF language-bar InitMenu must remove the upstream resource-menu load'
$zhIcon = Join-Path $overlayResource 'zh.ico'
$enIcon = Join-Path $overlayResource 'en.ico'
Assert-FamoLanguageIcon $zhIcon 'Chinese'
Assert-FamoLanguageIcon $enIcon 'English'
if ((Get-FileHash $zhIcon).Hash -eq (Get-FileHash $enIcon).Hash) {
  throw 'Chinese and English language-bar icons must be visually distinct assets'
}
$configZhIcon = Join-Path $configOverlayResource 'famo_zh.ico'
$configAsciiIcon = Join-Path $configOverlayResource 'famo_ascii.ico'
Assert-FamoLanguageIcon $configZhIcon 'config Chinese'
Assert-FamoLanguageIcon $configAsciiIcon 'config English'
if ((Get-FileHash $configZhIcon).Hash -eq (Get-FileHash $configAsciiIcon).Hash) {
  throw 'config Chinese and English language-bar icons must be visually distinct assets'
}

Assert-NotContains $statusPatch 'm_status_bar.OnSettings' 'floating status bar must not expose input settings as a persistent button'
Assert-NotContains $statusPatch 'L"刷新配置"' 'floating status-bar popup menus must not expose deploy/reload; settings owns maintenance'
Assert-NotContains $statusBarOverlay 'ButtonAction::Settings' 'persistent status-bar buttons must not include a settings action'
Assert-InOrder $statusBarOverlay @(
  '"ascii_mode"',
  '"ascii_punct"',
  '"traditionalization"',
  '"full_shape"',
  'ButtonAction::Expand'
) 'persistent status-bar buttons must keep only input-state toggles and ellipsis.'

Assert-InOrder $statusPatch @(
  '法墨右键菜单',
  'L"输入法设定"',
  'launch_famo_settings(dir, L"input")',
  'L"英文输入"',
  'L"关于"',
  'launch_famo_settings(dir, L"about")',
  'm_context_popup.SetItems'
) 'status-bar right-click menu must keep settings, state toggles, about and quit before setting its items.'

$expandStart = $statusPatch.IndexOf('法墨弹出面板', [System.StringComparison]::Ordinal)
$expandEnd = $statusPatch.IndexOf('m_popup.SetItems', $expandStart, [System.StringComparison]::Ordinal)
if ($expandStart -lt 0 -or $expandEnd -lt $expandStart) {
  throw 'status-bar patch must still contain the ellipsis popup block'
}
$expandPopupPatch = $statusPatch.Substring($expandStart, $expandEnd - $expandStart)
Assert-NotContains $expandPopupPatch 'L"刷新配置"' 'ellipsis popup must not expose deploy/reload; settings owns maintenance'
Assert-InOrder $statusPatch @(
  '法墨弹出面板',
  'L"输入法设定"',
  'launch_famo_settings(dir, L"input")',
  'L"英文输入"',
  'L"关于"',
  'launch_famo_settings(dir, L"about")',
  'm_popup.SetItems'
) 'status-bar expand menu must put input settings before input-state toggles and omit maintenance actions.'

Write-Host 'Famo tray menu macOS parity contract passed'
