$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$statusHeader = Get-Content (Join-Path $root 'overlay/include/FamoStatusBar.h') -Raw
$popupHeader = Get-Content (Join-Path $root 'overlay/include/FamoPopupPanel.h') -Raw
$statusSource = Get-Content (Join-Path $root 'overlay/WeaselUI/FamoStatusBar.cpp') -Raw
$popupSource = Get-Content (Join-Path $root 'overlay/WeaselUI/FamoPopupPanel.cpp') -Raw
$statusPatch = Get-Content (Join-Path $root 'features/status-bar.patch') -Raw
$applyScript = Get-Content (Join-Path $root 'apply-famo-statusbar.ps1') -Raw

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

Assert-Contains $statusHeader 'MESSAGE_HANDLER(WM_MOUSELEAVE, OnMouseLeave)' 'status bar must clear hover on WM_MOUSELEAVE'
Assert-Contains $statusHeader 'MESSAGE_HANDLER(WM_CAPTURECHANGED, OnCaptureChanged)' 'status bar must clear press state when capture is lost'
Assert-Contains $statusHeader 'MESSAGE_HANDLER(WM_RBUTTONUP, OnRButtonUp)' 'status bar must handle right-click release'
Assert-Contains $statusHeader 'MESSAGE_HANDLER(WM_NCHITTEST, OnNcHitTest)' 'status bar must force its floating rectangle to be mouse-hit-testable'
Assert-Contains $popupHeader 'MESSAGE_HANDLER(WM_NCHITTEST, OnNcHitTest)' 'status-bar popup menu must force its layered rectangle to be mouse-hit-testable'
Assert-Contains $popupHeader 'MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLButtonDown)' 'status-bar popup menu must track left-button press before dispatching a row command'
Assert-Contains $popupHeader 'MESSAGE_HANDLER(WM_RBUTTONUP, OnRButtonUp)' 'status-bar popup menu must also accept right-button menu selection semantics'
Assert-Contains $statusHeader 'WS_POPUP | WS_CLIPSIBLINGS,' 'status bar must be an enabled popup window so mouse clicks reach the icon buttons'
Assert-Contains $statusHeader 'WS_EX_TOPMOST' 'status bar must stay above foreground apps so clicks reach the floating bar instead of covered windows'
Assert-NotContains $statusHeader 'WS_DISABLED' 'status bar must not be a disabled window; disabled windows do not reliably receive user mouse clicks'
Assert-Contains $popupHeader 'WS_POPUP | WS_CLIPSIBLINGS,' 'status-bar popup panels must be enabled so custom menu rows receive clicks'
Assert-NotContains $popupHeader 'WS_DISABLED' 'status-bar popup panels must not be disabled windows; custom menu rows need mouse clicks'
Assert-Contains $statusSource '"ascii_mode", &opt_ascii_mode_' 'first status icon must map to the real ascii_mode option'
Assert-Contains $statusSource '"ascii_punct", &opt_ascii_punct_' 'punctuation status icon must map to the real ascii_punct option'
Assert-Contains $statusSource '"traditionalization", &opt_traditionalization_' 'simplified/traditional status icon must map to the real traditionalization option'
Assert-Contains $statusSource '"zh_trad"' 'simplified/traditional status icon must also fan out to the Wubi zh_trad option'
Assert-Contains $statusSource '"full_shape", &opt_full_shape_' 'half/full-shape status icon must map to the real full_shape option'
Assert-Contains $statusSource '_DrawExpandGlyph(b.rc, hover, press);' 'status-bar expand button must use the lightweight vector glyph'
Assert-Contains $statusSource 'FillEllipse' 'status-bar expand glyph must be drawn with vector primitives (three-dot glyph, b929b86)'
Assert-NotContains $statusSource 'DrawBitmap(icon_bmp_.Get()' 'status-bar expand button must not draw the full PNG badge'
Assert-Contains $statusSource 'interaction_.LeftUp(ClientPoint(lParam), was_drag)' 'left click must route through interaction model'
Assert-Contains $statusSource 'ToggleOption(b.option);' 'left-clicking a status icon must use the same real option-toggle path as menus'
Assert-Contains $statusSource 'setter_("zh_trad", *b.state);' 'toggling simplified/traditional must set Wubi zh_trad with the same state'
Assert-Contains $statusSource 'on_right_click_(AnchorTopLeftScreen())' 'right click must call injected menu callback'
Assert-Contains $statusSource 'return HTCLIENT;' 'status bar hit-test must return HTCLIENT so the layered popup can receive mouse messages'
Assert-Contains $statusSource 'SetWindowPos(HWND_TOPMOST' 'status bar must keep its topmost z-order when shown or dragged'
Assert-Contains $statusSource 'if (!memDC || !memBmp)' 'status bar paint must tolerate failed compatible DC/bitmap creation'
Assert-Contains $statusSource 'if (!dwr_ || !dwr_->pRenderTarget)' 'status bar paint must not dereference missing Direct2D resources'
Assert-Contains $statusSource 'HRESULT hr = rt->BindDC(memDC, &rc);' 'status bar paint must check BindDC before drawing'
Assert-Contains $statusSource 'hr = rt->EndDraw();' 'status bar paint must check EndDraw before UpdateLayeredWindow'
Assert-Contains $popupSource 'return HTCLIENT;' 'status-bar popup menu hit-test must return HTCLIENT so layered menu rows receive mouse messages'
Assert-Contains $popupSource 'interaction_.LeftUp(' 'status-bar popup menu must hit-test rows (via the shared interaction model) before invoking actions'
Assert-Contains $popupSource '_InvokeRow(click.index)' 'status-bar popup menu must invoke the selected row after a valid button release'
Assert-Contains $popupSource 'click.has_value' 'status-bar popup menu must tolerate missing down messages but avoid firing a different pressed row'
Assert-Contains $popupSource 'if (!memDC || !memBmp)' 'status-bar popup paint must tolerate failed compatible DC/bitmap creation'
Assert-Contains $popupSource 'HRESULT hr = rt->BindDC(memDC, &rc);' 'status-bar popup paint must check BindDC before drawing'
Assert-Contains $popupSource 'hr = rt->EndDraw();' 'status-bar popup paint must check EndDraw before UpdateLayeredWindow'
Assert-InOrder $statusSource @(
  'const bool was_drag = m_dragging;',
  'FamoStatusBarClick click = interaction_.LeftUp(ClientPoint(lParam), was_drag)',
  'if (::GetCapture() == m_hWnd)',
  '::ReleaseCapture();'
) 'status bar must decide the click before ReleaseCapture triggers WM_CAPTURECHANGED and clears press state.'

Assert-Contains $statusPatch 'FamoPopupPanel m_context_popup;' 'WeaselServerApp must own a distinct right-click popup panel'
Assert-Contains $statusPatch 'm_status_bar.OnRightClick' 'WeaselServerApp must wire status bar right-click callback'
Assert-Contains $statusPatch 'm_handler->SetOption(0, opt, val)' 'status-bar option toggles must reach the real RimeWithWeaselHandler option setter'
Assert-Contains $statusPatch 'm_context_popup.SetItems' 'right-click callback must set real menu items'
Assert-Contains $statusPatch '<ClInclude Include="..\include\FamoStatusBarInteraction.h" />' 'WeaselUI project must include the interaction model header'
Assert-Contains $applyScript 'include/FamoStatusBarInteraction.h' 'apply script must copy interaction model header into upstream checkout'
Assert-Contains $applyScript 'famo-statusbar-dryrun-' 'dry-run must use a temporary worktree so dependent patches are simulated in order'
Assert-Contains $applyScript 'Copy-Item -LiteralPath $UpstreamDir' 'dry-run must copy the current working tree before applying dependent patches'
Assert-NotContains $applyScript 'famo.png -> output/famo.png' 'status-bar apply script must not copy the old PNG badge'

Write-Host 'Famo status-bar overlay contract passed'
