using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Microsoft.UI.Text;

namespace Famo.Settings.Theming;

/// <summary>
/// 设计稿组件的代码构建器（卡片/行/药丸/勾选/分段/步进/主按钮/状态行/横幅）。色与样式取运行时
/// 注入的 Famo.* 笔刷与 FamoStyles.xaml，使各页用极少代码即铺出「Claude Design look」并随换肤重绘。
/// </summary>
public static class FamoUI
{
    private static T R<T>(string key) where T : class =>
        (FamoTheme.FindRes(Application.Current.Resources, key) as T)!;

    public static SolidColorBrush Br(string key) =>
        R<SolidColorBrush>(key) ?? new SolidColorBrush(Microsoft.UI.Colors.Gray);

    private static Style St(string key) => R<Style>(key);

    public static readonly FontFamily Serif = R<FontFamily>("Famo.Serif");
    public static readonly FontFamily Mono = R<FontFamily>("Famo.Mono");

    // ── 页头 ──
    public static StackPanel PaneHeader(string title, string desc)
    {
        var sp = new StackPanel { Margin = new Thickness(0, 0, 0, 22) };
        sp.Children.Add(new TextBlock { Text = title, Style = St("FamoPaneTitle") });
        if (!string.IsNullOrEmpty(desc))
            sp.Children.Add(new TextBlock { Text = desc, Style = St("FamoPaneDesc"), MaxWidth = 520 });
        return sp;
    }

    // ── 卡片（衬线标题 + 行）──
    public static Border Card(string title, params UIElement[] rows)
    {
        var inner = new StackPanel();
        if (!string.IsNullOrEmpty(title))
            inner.Children.Add(new TextBlock { Text = title, Style = St("FamoSectionTitle") });
        bool first = true;
        foreach (UIElement r in rows)
        {
            if (r is FrameworkElement fe && !first && fe.Tag as string != "nodivider")
                fe.Margin = new Thickness(0); // 分隔线由 Row 自带顶边框
            inner.Children.Add(r);
            first = false;
        }
        return new Border { Style = St("FamoCard"), Child = inner, Margin = new Thickness(0, 0, 0, 22) };
    }

    // ── 普通行：label(标题+说明) | 控件 ──
    public static Grid Row(string title, string? desc, FrameworkElement control, bool divider = true)
    {
        var g = new Grid { Padding = new Thickness(0, 11, 0, 11) };
        if (divider)
        {
            g.BorderBrush = Br("Famo.Line2");
            g.BorderThickness = new Thickness(0, 1, 0, 0);
        }
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        var lab = new StackPanel { VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 22, 0) };
        lab.Children.Add(new TextBlock { Text = title, Style = St("FamoRowTitle") });
        if (!string.IsNullOrEmpty(desc))
            lab.Children.Add(new TextBlock { Text = desc, Style = St("FamoRowDesc") });
        Grid.SetColumn(lab, 0);
        g.Children.Add(lab);

        control.VerticalAlignment = VerticalAlignment.Center;
        control.HorizontalAlignment = HorizontalAlignment.Right;
        Grid.SetColumn(control, 1);
        g.Children.Add(control);
        return g;
    }

    // ── 整宽行（预览/方案列表/皮肤卡）──
    public static FrameworkElement RowFull(FrameworkElement content, bool divider = false)
    {
        content.Margin = new Thickness(0, divider ? 12 : 0, 0, 12);
        if (divider && content is Control c) c.BorderThickness = new Thickness(0, 1, 0, 0);
        return content;
    }

    // ── 药丸开关 ──
    public static ToggleButton Pill(bool isOn, Action<bool> changed)
    {
        var t = new ToggleButton { Style = St("FamoPill"), IsChecked = isOn };
        t.Checked += (_, _) => changed(true);
        t.Unchecked += (_, _) => changed(false);
        return t;
    }

    // ── 方形勾选 ──
    public static ToggleButton Check(bool isOn, Action<bool> changed)
    {
        var t = new ToggleButton { Style = St("FamoCheck"), IsChecked = isOn };
        t.Checked += (_, _) => changed(true);
        t.Unchecked += (_, _) => changed(false);
        return t;
    }

    // ── 分段（单选）──
    public static Border SegBar(string[] options, int selected, Action<int> changed)
    {
        var bar = new Border { Style = St("FamoSegBar") };
        var sp = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 2 };
        var items = new List<ToggleButton>();
        for (int i = 0; i < options.Length; i++)
        {
            int idx = i;
            var b = new ToggleButton { Style = St("FamoSegItem"), Content = options[i], IsChecked = i == selected };
            b.Click += (_, _) =>
            {
                for (int k = 0; k < items.Count; k++) items[k].IsChecked = k == idx;
                changed(idx);
            };
            items.Add(b);
            sp.Children.Add(b);
        }
        bar.Child = sp;
        return bar;
    }

    // ── 步进 − v + ──
    public static Border Stepper(int val, int min, int max, Action<int> changed)
    {
        var bar = new Border
        {
            Background = Br("Famo.Field"),
            BorderBrush = Br("Famo.Line"),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(7),
            Height = 32,
        };
        var sp = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
        var vt = new TextBlock { Text = val.ToString(), FontFamily = Mono, FontSize = 14, Foreground = Br("Famo.Ink"), MinWidth = 30, TextAlignment = TextAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        int cur = val;
        Button mk(string g, int d) => new()
        {
            Content = g, Width = 30, Height = 30, FontSize = 16, Padding = new Thickness(0),
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), BorderThickness = new Thickness(0),
            Foreground = Br("Famo.Ink"),
        };
        var minus = mk("−", -1);
        var plus = mk("+", 1);
        void upd()
        {
            vt.Text = cur.ToString();
            minus.IsEnabled = cur > min;
            plus.IsEnabled = cur < max;
        }
        minus.Click += (_, _) => { if (cur > min) { cur--; upd(); changed(cur); } };
        plus.Click += (_, _) => { if (cur < max) { cur++; upd(); changed(cur); } };
        upd();
        sp.Children.Add(minus); sp.Children.Add(vt); sp.Children.Add(plus);
        bar.Child = sp;
        return bar;
    }

    // ── 主按钮 ──
    public static Button FilledButton(string text, string? glyph, Action click)
    {
        var content = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, HorizontalAlignment = HorizontalAlignment.Center };
        if (!string.IsNullOrEmpty(glyph))
            content.Children.Add(new FontIcon { Glyph = glyph, FontSize = 15, Foreground = Br("Famo.OnAccent") });
        content.Children.Add(new TextBlock { Text = text, VerticalAlignment = VerticalAlignment.Center });
        var b = new Button { Style = St("FamoFilled"), Content = content, Margin = new Thickness(0, 4, 0, 22) };
        b.Click += (_, _) => click();
        return b;
    }

    public static Button ActionButton(string text, Action click)
    {
        var b = new Button { Content = text, MinWidth = 72 };
        b.Click += (_, _) => click();
        return b;
    }

    // ── 二次确认对话框（用于删除等不可逆操作）──
    public static async Task<bool> Confirm(XamlRoot root, string title, string message)
    {
        var dialog = new ContentDialog
        {
            Title = title,
            Content = new TextBlock { Text = message, TextWrapping = TextWrapping.Wrap },
            PrimaryButtonText = "删除",
            CloseButtonText = "取消",
            DefaultButton = ContentDialogButton.Close,
            XamlRoot = root,
        };
        return await dialog.ShowAsync() == ContentDialogResult.Primary;
    }

    // ── 状态行 ──
    public static Border StatusRow(string text)
    {
        var sp = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 9, VerticalAlignment = VerticalAlignment.Center };
        sp.Children.Add(new Ellipse { Width = 7, Height = 7, Fill = Br("Famo.StatusGreen"), VerticalAlignment = VerticalAlignment.Center });
        sp.Children.Add(new TextBlock { Text = text, FontSize = 12, Foreground = Br("Famo.Ink2"), VerticalAlignment = VerticalAlignment.Center, TextWrapping = TextWrapping.Wrap });
        return new Border { Style = St("FamoStatus"), Child = sp, Margin = new Thickness(0, 0, 0, 22) };
    }

    // ── 横幅：即时 / 部署 ──
    public static Border Banner(bool deploy, string text)
    {
        var sp = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12, VerticalAlignment = VerticalAlignment.Center };
        if (deploy)
            sp.Children.Add(new Ellipse { Width = 8, Height = 8, Fill = Br("Famo.Accent"), VerticalAlignment = VerticalAlignment.Center });
        else
            sp.Children.Add(new TextBlock { Text = "即时", FontSize = 12, FontWeight = FontWeights.SemiBold, Foreground = Br("Famo.Accent"), VerticalAlignment = VerticalAlignment.Center });
        sp.Children.Add(new TextBlock { Text = text, FontSize = 12, Foreground = deploy ? Br("Famo.Ink") : Br("Famo.Ink2"), VerticalAlignment = VerticalAlignment.Center, TextWrapping = TextWrapping.Wrap });
        return new Border
        {
            Background = deploy ? Br("Famo.BannerDeployBg") : Br("Famo.BannerInstantBg"),
            BorderBrush = deploy ? Br("Famo.BannerDeployBorder") : Br("Famo.Line2"),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(13),
            Padding = new Thickness(16, 11, 16, 11),
            Margin = new Thickness(0, 0, 0, 22),
            Child = sp,
        };
    }

    // ── 只读值 ──
    public static TextBlock Value(string text) => new()
    {
        Text = text, FontFamily = Mono, FontSize = 12, Foreground = Br("Famo.Ink2"),
        TextAlignment = TextAlignment.Right, MaxWidth = 230, TextWrapping = TextWrapping.Wrap,
    };
}
