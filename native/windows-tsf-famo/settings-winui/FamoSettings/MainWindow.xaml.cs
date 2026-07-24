using Famo.Settings.Core;
using Famo.Settings.Theming;
using Famo.Settings.Views;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Text;

namespace Famo.Settings;

/// <summary>
/// 法墨设置主窗：侧栏单字徽标导航 + 右侧分组卡片详情（对标 macOS「Claude Design」）。
/// </summary>
public sealed partial class MainWindow : Window
{
    private sealed record NavDef(string Id, string Title, string Glyph);

    private sealed record NavVisual(Button Root, Border Badge, FontIcon BadgeIcon, TextBlock Title);

    private readonly Dictionary<string, NavVisual> _items = new();
    private string _current = "";

    private static readonly NavDef[] Pages = SettingsNavigation.VisiblePages
        .Select(page => new NavDef(page.Id, page.Title, page.Glyph))
        .ToArray();

    public MainWindow(string? startPage = null)
    {
        this.InitializeComponent();
        Title = "法墨设置";
        TrySetBrandIcon(); // 标题栏/任务栏图标 = 法墨墨滴（对齐 macOS 版）
        BuildNav();
        FamoTheme.Changed += ReskinNav; // 换肤时刷新选中态配色
        Select(ResolveStartPage(startPage)); // 起始页：--page 深链 / 默认候选页
    }

    /// <summary>外部（单实例重定向）导航到 --page 深链指定页。</summary>
    public void NavigateTo(string? page) => Select(ResolveStartPage(page));

    /// <summary>把 --page 深链 id 解析成有效导航 id；未知/缺省落键盘输入页。</summary>
    private static string ResolveStartPage(string? page) => SettingsNavigation.ResolvePageId(page);

    /// <summary>把窗口/任务栏图标设为法墨品牌墨滴 ico（unpackaged WinUI 需经 AppWindow.SetIcon）。失败静默。</summary>
    private void TrySetBrandIcon()
    {
        try
        {
            string ico = System.IO.Path.Combine(AppContext.BaseDirectory, "Assets", "famo.ico");
            if (!System.IO.File.Exists(ico)) return;
            nint hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
            Microsoft.UI.WindowId id = Microsoft.UI.Win32Interop.GetWindowIdFromWindow(hwnd);
            Microsoft.UI.Windowing.AppWindow.GetFromWindowId(id)?.SetIcon(ico);
        }
        catch
        {
            // 取不到 AppWindow / 文件缺失不应阻断启动。
        }
    }

    private void BuildNav()
    {
        var panel = new StackPanel { Spacing = 3 };
        foreach (NavDef def in Pages)
            panel.Children.Add(BuildNavItem(def));
        NavHost.Children.Add(panel);
    }

    private Button BuildNavItem(NavDef def)
    {
        var badgeIcon = new FontIcon
        {
            Glyph = def.Glyph, FontSize = 14,
            Foreground = FamoUI.Br("Famo.Ink2"),
            HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
        };
        var badge = new Border
        {
            Width = 24, Height = 24, CornerRadius = new CornerRadius(6),
            Background = FamoUI.Br("Famo.Card2"), Child = badgeIcon,
        };
        var title = new TextBlock
        {
            Text = def.Title, FontSize = 14, Foreground = FamoUI.Br("Famo.Ink"),
            VerticalAlignment = VerticalAlignment.Center,
        };
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        row.Children.Add(badge);
        row.Children.Add(title);

        var root = new Button
        {
            CornerRadius = new CornerRadius(8), Padding = new Thickness(8, 5, 8, 5),
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
            BorderThickness = new Thickness(0),
            Content = row,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
        };
        AutomationProperties.SetName(root, def.Title);
        root.Click += (_, _) => Select(def.Id);
        root.PointerEntered += (_, _) => { if (_current != def.Id) root.Background = FamoUI.Br("Famo.Hover"); };
        root.PointerExited += (_, _) => { if (_current != def.Id) root.Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent); };

        _items[def.Id] = new NavVisual(root, badge, badgeIcon, title);
        return root;
    }

    private void Select(string id)
    {
        string pageId = ResolveStartPage(id);
        _current = SettingsNavigation.VisibleParentPageId(pageId);
        ReskinNav();
        PageHost.Content = BuildPage(pageId);
        ContentScroller.ChangeView(null, 0, null, true);
    }

    private static FrameworkElement BuildPage(string id) => id switch
    {
        "keyboard" => new KeyboardPage(),
        "shortcuts" => new ShortcutsPage(),
        "candidate" => new CandidatePage(),
        "quick-phrases" => new QuickPhrasesPage(),
        "prompt-library" => new PromptLibraryPage(),
        "clipboard" => new ClipboardPage(),
        "skills" => new SkillsPage(),
        "ai" => new AiPage(),
        "status-bar" => new StatusBarPage(),
        "skin" => new SkinPage(),
        "about" => new AboutPage(),
        _ => new KeyboardPage(),
    };

    /// <summary>按当前选中 + 皮肤刷新导航配色。</summary>
    private void ReskinNav()
    {
        var transparent = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        foreach ((string id, NavVisual v) in _items)
        {
            bool sel = id == _current;
            v.Root.Background = sel ? FamoUI.Br("Famo.Accent") : transparent;
            v.Badge.Background = sel ? FamoUI.Br("Famo.OnAccent20") : FamoUI.Br("Famo.Card2");
            v.BadgeIcon.Foreground = sel ? FamoUI.Br("Famo.OnAccent") : FamoUI.Br("Famo.Ink2");
            v.Title.Foreground = sel ? FamoUI.Br("Famo.OnAccent") : FamoUI.Br("Famo.Ink");
            v.Title.FontWeight = sel ? FontWeights.SemiBold : FontWeights.Normal;
        }
    }
}
