using System.Runtime.InteropServices;
using Famo.Settings.Core.Emoji;
using Famo.Settings.Interop;
using Famo.Settings.Theming;
using Microsoft.UI.Text;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;

namespace Famo.Settings.Views;

/// <summary>
/// 表情/符号浮窗（学搜狗：状态栏触发拉起）。无边框 / 置顶 / **不抢焦点**
/// （WS_EX_NOACTIVATE + SW_SHOWNOACTIVATE），故焦点仍在目标 app，点选即用 SendInput
/// 上屏到其输入框。分类 tab + 搜索 + 网格（颜文字整行）+「最近」（去重 cap14）。
/// </summary>
public sealed class EmojiWindow : Window
{
    private const int Columns = 8;
    private const double Tile = 40;

    private readonly EmojiRecentsStore _recents = new();
    private VariableSizedWrapGrid _grid = null!;
    private TextBox _search = null!;
    private StackPanel _tabs = null!;
    private FamoEmojiCategory _category = FamoEmojiCategory.Recent;

    public EmojiWindow()
    {
        Title = "法墨表情";
        // 注意：不在 ctor 访问 AppWindow（code-only 窗在构造期 AppWindow 尚未就绪）。
        BuildContent();
    }

    private void BuildContent()
    {
        var root = new Grid
        {
            Background = FamoUI.Br("Famo.Card"),
            Padding = new Thickness(10),
            RowDefinitions =
            {
                new RowDefinition { Height = GridLength.Auto }, // tabs
                new RowDefinition { Height = GridLength.Auto }, // search
                new RowDefinition { Height = new GridLength(1, GridUnitType.Star) }, // grid
            },
        };

        // _search 必须先于 BuildTabs 创建（BuildTabs 读 _search.Text 判选中态）。
        _search = new TextBox
        {
            PlaceholderText = "搜索表情 / 符号…",
            Margin = new Thickness(0, 0, 0, 8),
            Background = FamoUI.Br("Famo.Field"),
            BorderBrush = FamoUI.Br("Famo.Line"),
            Foreground = FamoUI.Br("Famo.Ink"),
        };
        _search.TextChanged += (_, _) => Render();

        _tabs = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4, Margin = new Thickness(0, 0, 0, 8) };
        BuildTabs();
        Grid.SetRow(_tabs, 0);
        root.Children.Add(_tabs);

        Grid.SetRow(_search, 1);
        root.Children.Add(_search);

        _grid = new VariableSizedWrapGrid
        {
            Orientation = Orientation.Horizontal,
            ItemWidth = Tile,
            ItemHeight = Tile,
            MaximumRowsOrColumns = Columns,
        };
        var scroller = new ScrollViewer
        {
            Content = _grid,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
        };
        Grid.SetRow(scroller, 2);
        root.Children.Add(scroller);

        Content = root;
        FamoTheme.Changed += () => { try { root.Background = FamoUI.Br("Famo.Card"); Render(); } catch { } };
        Render();
        // 注意：不在 ctor 内 Activate（构造期激活会崩 0xc000027b）。首次显示在 ShowNearCursor
        // 用 this.Activate() 创建 XAML island 并显示；WS_EX_NOACTIVATE 已设，故不抢前台焦点。
    }

    /// <summary>在光标附近显示（不抢焦点）。每次拉起刷新「最近」与定位。</summary>
    public void ShowNearCursor()
    {
        _search.Text = "";
        _category = FamoEmojiCategory.Recent;
        BuildTabs();
        Render();
        this.Activate();              // 先显示（AppWindow 就绪 + 渲染 island；MainWindow 同样模式）
        if (!_configured) { ConfigurePresenter(); _configured = true; } // 显示后再配置表现
        PositionNearCursor();
        ApplyNoActivate();            // 之后不再抢前台焦点（焦点留目标 app，供 SendInput 上屏）
    }

    private bool _configured;

    public void HideWindow() => AppWindow.Hide();

    // ── 构建分类 tab ──
    private void BuildTabs()
    {
        _tabs.Children.Clear();
        foreach (FamoEmojiCategory cat in System.Enum.GetValues<FamoEmojiCategory>())
        {
            FamoEmojiCategory c = cat;
            bool sel = c == _category && _search.Text.Length == 0;
            var t = new TextBlock
            {
                Text = FamoEmojiCategoryInfo.Label(c),
                FontSize = 12.5,
                FontWeight = sel ? FontWeights.SemiBold : FontWeights.Normal,
                Foreground = sel ? FamoUI.Br("Famo.OnAccent") : FamoUI.Br("Famo.Ink2"),
                VerticalAlignment = VerticalAlignment.Center,
            };
            var b = new Border
            {
                CornerRadius = new CornerRadius(7),
                Padding = new Thickness(9, 4, 9, 4),
                Background = sel ? FamoUI.Br("Famo.Accent") : new SolidColorBrush(Microsoft.UI.Colors.Transparent),
                Child = t,
            };
            b.PointerPressed += (_, _) =>
            {
                _category = c;
                _search.Text = "";
                BuildTabs();
                Render();
            };
            _tabs.Children.Add(b);
        }
    }

    // ── 渲染网格（搜索优先，否则当前分类；最近来自 store）──
    private void Render()
    {
        _grid.Children.Clear();
        IReadOnlyList<FamoGlyph> items =
            _search.Text.Trim().Length > 0 ? FamoEmojiData.Search(_search.Text)
            : _category == FamoEmojiCategory.Recent ? _recents.Glyphs()
            : FamoEmojiData.ItemsFor(_category);

        foreach (FamoGlyph g in items)
            _grid.Children.Add(BuildTile(g));
    }

    private UIElement BuildTile(FamoGlyph glyph)
    {
        var text = new TextBlock
        {
            Text = glyph.Char,
            FontSize = glyph.Wide ? 14 : 20,
            Foreground = FamoUI.Br("Famo.Ink"),
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            TextAlignment = TextAlignment.Center,
            TextTrimming = TextTrimming.CharacterEllipsis,
        };
        if (glyph.Wide) text.FontFamily = FamoUI.Mono; // 颜文字用等宽；其余留默认字体（勿设 null）
        var b = new Border
        {
            CornerRadius = new CornerRadius(7),
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
            Child = text,
            Margin = new Thickness(2),
            Padding = new Thickness(2),
        };
        if (glyph.Wide)
            VariableSizedWrapGrid.SetColumnSpan(b, Columns);

        b.PointerEntered += (_, _) => b.Background = FamoUI.Br("Famo.Hover");
        b.PointerExited += (_, _) => b.Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        b.PointerPressed += (_, _) => Pick(glyph.Char);
        return b;
    }

    private void Pick(string ch)
    {
        _recents.Push(ch);
        HideWindow();               // 先收起浮窗（焦点本就在目标 app）
        TextInjector.Inject(ch);    // 再上屏到焦点输入框
    }

    // ── 窗口表现：无边框 / 置顶 / 不抢焦点 / 不进 Alt-Tab ──
    private void ConfigurePresenter()
    {
        if (AppWindow.Presenter is OverlappedPresenter p)
        {
            p.SetBorderAndTitleBar(true, false);
            p.IsAlwaysOnTop = true;
            p.IsResizable = false;
            p.IsMaximizable = false;
            p.IsMinimizable = false;
        }
        AppWindow.Resize(new Windows.Graphics.SizeInt32(380, 460));

        // 仅 TOOLWINDOW（不进 Alt-Tab）。WS_EX_NOACTIVATE 不在此处设——若在首次显示前就置上，
        // WinUI/AppWindow 会让窗保持隐藏（实测 IsWindowVisible=False）。改在首次显示后再加。
        nint hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        int ex = GetWindowLong(hwnd, GWL_EXSTYLE);
        SetWindowLong(hwnd, GWL_EXSTYLE, ex | WS_EX_TOOLWINDOW);
    }

    /// <summary>首次显示后追加 WS_EX_NOACTIVATE，使后续不再抢前台焦点（焦点留目标 app）。</summary>
    private void ApplyNoActivate()
    {
        nint hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        int ex = GetWindowLong(hwnd, GWL_EXSTYLE);
        SetWindowLong(hwnd, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE);
    }

    private void PositionNearCursor()
    {
        if (!GetCursorPos(out POINT pt)) return;
        DisplayArea area = DisplayArea.GetFromPoint(
            new Windows.Graphics.PointInt32(pt.X, pt.Y), DisplayAreaFallback.Nearest);
        Windows.Graphics.RectInt32 work = area.WorkArea;
        Windows.Graphics.SizeInt32 size = AppWindow.Size;

        int x = pt.X + 12;
        int y = pt.Y + 12;
        if (x + size.Width > work.X + work.Width) x = pt.X - size.Width - 12;
        if (y + size.Height > work.Y + work.Height) y = work.Y + work.Height - size.Height;
        if (x < work.X) x = work.X;
        if (y < work.Y) y = work.Y;
        AppWindow.Move(new Windows.Graphics.PointInt32(x, y));
    }

    // ── Win32 interop ──
    private const int GWL_EXSTYLE = -20;
    private const int WS_EX_NOACTIVATE = 0x08000000;
    private const int WS_EX_TOOLWINDOW = 0x00000080;

    [StructLayout(LayoutKind.Sequential)]
    private struct POINT { public int X; public int Y; }

    [DllImport("user32.dll")] private static extern bool GetCursorPos(out POINT lpPoint);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongW")] private static extern int GetWindowLong(nint hWnd, int nIndex);
    [DllImport("user32.dll", EntryPoint = "SetWindowLongW")] private static extern int SetWindowLong(nint hWnd, int nIndex, int dwNewLong);
}
