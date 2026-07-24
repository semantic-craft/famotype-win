using System.Runtime.InteropServices;
using Famo.Settings.Core.Insertion;
using Famo.Settings.Core.QuickPhrases;
using Famo.Settings.Theming;
using Microsoft.UI.Text;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.System;

namespace Famo.Settings.Views;

/// <summary>快捷短语选择器：显式打开，避免五笔字母编码被快捷短语候选占用。</summary>
public sealed class QuickPhrasePickerWindow : Window
{
    private readonly QuickPhraseStore _store = new();
    private readonly ITextInsertionService _insertion;
    private IReadOnlyList<QuickPhraseEntry> _entries = Array.Empty<QuickPhraseEntry>();
    private TextBox _search = null!;
    private StackPanel _list = null!;
    private TextBlock _preview = null!;
    private TextBlock _status = null!;
    private bool _configured;
    private Grid _root = null!;

    public QuickPhrasePickerWindow(ITextInsertionService insertion)
    {
        _insertion = insertion;
        Title = "法墨快捷短语";
        BuildContent();
    }

    private void BuildContent()
    {
        var root = new Grid
        {
            Background = FamoUI.Br("Famo.Card"),
            Padding = new Thickness(12),
            RowDefinitions =
            {
                new RowDefinition { Height = GridLength.Auto },
                new RowDefinition { Height = GridLength.Auto },
                new RowDefinition { Height = new GridLength(1, GridUnitType.Star) },
                new RowDefinition { Height = GridLength.Auto },
            },
        };

        var header = new StackPanel { Margin = new Thickness(0, 0, 0, 10) };
        header.Children.Add(new TextBlock
        {
            Text = "快捷短语",
            FontSize = 18,
            FontWeight = FontWeights.SemiBold,
            Foreground = FamoUI.Br("Famo.Ink"),
        });
        _status = new TextBlock
        {
            Text = "搜索短语或编码，回车插入；Esc 关闭。",
            FontSize = 12,
            Foreground = FamoUI.Br("Famo.Ink2"),
            TextWrapping = TextWrapping.Wrap,
        };
        header.Children.Add(_status);
        Grid.SetRow(header, 0);
        root.Children.Add(header);

        _search = new TextBox
        {
            PlaceholderText = "搜索快捷短语",
            MinWidth = 300,
            Margin = new Thickness(0, 0, 0, 10),
        };
        _search.TextChanged += (_, _) => RenderList();
        _search.KeyDown += SearchKeyDown;
        Grid.SetRow(_search, 1);
        root.Children.Add(_search);

        _list = new StackPanel { Spacing = 8 };
        var scroller = new ScrollViewer
        {
            Content = _list,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
        };
        Grid.SetRow(scroller, 2);
        root.Children.Add(scroller);

        _preview = new TextBlock
        {
            Text = "选择一个短语查看预览。",
            FontSize = 12.5,
            Foreground = FamoUI.Br("Famo.Ink2"),
            TextWrapping = TextWrapping.Wrap,
            MaxLines = 3,
            Margin = new Thickness(0, 10, 0, 0),
        };
        Grid.SetRow(_preview, 3);
        root.Children.Add(_preview);

        Content = root;
        _root = root;
        FamoTheme.Changed += OnThemeChanged;
    }

    public void UnsubscribeTheme() => FamoTheme.Changed -= OnThemeChanged;

    private void OnThemeChanged()
    {
        try { _root.Background = FamoUI.Br("Famo.Card"); RenderList(); } catch { }
    }

    public void ShowNearCursor()
    {
        _entries = _store.Load();
        RenderList();
        Activate();
        if (!_configured) { ConfigurePresenter(); _configured = true; }
        PositionNearCursor();
        _search.Focus(FocusState.Programmatic);
    }

    private void RenderList()
    {
        if (_list is null) return;
        _list.Children.Clear();

        IReadOnlyList<QuickPhraseEntry> entries = FilterEntries();
        if (entries.Count == 0)
        {
            _list.Children.Add(new TextBlock
            {
                Text = "暂无匹配短语",
                FontSize = 13,
                Foreground = FamoUI.Br("Famo.Ink2"),
                Margin = new Thickness(0, 8, 0, 0),
            });
            return;
        }

        foreach (QuickPhraseEntry entry in entries)
        {
            _list.Children.Add(BuildEntryRow(entry));
        }
    }

    private IReadOnlyList<QuickPhraseEntry> FilterEntries()
    {
        string query = (_search?.Text ?? string.Empty).Trim();
        if (query.Length == 0) return _entries;

        return _entries
            .Where(entry =>
                entry.Text.Contains(query, StringComparison.CurrentCultureIgnoreCase)
                || entry.Code.Contains(query, StringComparison.OrdinalIgnoreCase)
                || ("v" + entry.Code).Contains(query, StringComparison.OrdinalIgnoreCase))
            .ToArray();
    }

    private UIElement BuildEntryRow(QuickPhraseEntry entry)
    {
        var title = new TextBlock
        {
            Text = entry.Text,
            FontSize = 13.5,
            FontWeight = FontWeights.SemiBold,
            Foreground = FamoUI.Br("Famo.Ink"),
            TextWrapping = TextWrapping.Wrap,
            MaxLines = 2,
        };
        var code = new TextBlock
        {
            Text = entry.Code + " / v" + entry.Code,
            FontFamily = FamoUI.Mono,
            FontSize = 12,
            Foreground = FamoUI.Br("Famo.Ink3"),
            TextWrapping = TextWrapping.NoWrap,
        };
        var stack = new StackPanel { Spacing = 3 };
        stack.Children.Add(title);
        stack.Children.Add(code);

        var item = new Border
        {
            CornerRadius = new CornerRadius(7),
            Background = FamoUI.Br("Famo.Field"),
            BorderBrush = FamoUI.Br("Famo.Line2"),
            BorderThickness = new Thickness(1),
            Padding = new Thickness(10),
            Child = stack,
            Tag = entry,
        };
        item.PointerEntered += (_, _) =>
        {
            item.Background = FamoUI.Br("Famo.Hover");
            _preview.Text = entry.Text;
        };
        item.PointerExited += (_, _) => item.Background = FamoUI.Br("Famo.Field");
        item.PointerPressed += async (_, _) => await InsertQuickPhraseAsync(entry);
        return item;
    }

    private async void SearchKeyDown(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key == VirtualKey.Escape)
        {
            HideWindow();
            e.Handled = true;
            return;
        }

        if (e.Key != VirtualKey.Enter) return;
        QuickPhraseEntry? entry = _list.Children
            .OfType<Border>()
            .Select(border => border.Tag as QuickPhraseEntry)
            .FirstOrDefault(item => item is not null);
        if (entry is null) return;
        e.Handled = true;
        await InsertQuickPhraseAsync(entry);
    }

    private async Task InsertQuickPhraseAsync(QuickPhraseEntry entry)
    {
        HideWindow();
        TextInsertionResult result = await _insertion.InsertAsync(entry.Text, CancellationToken.None);
        if (!result.Success)
        {
            Activate();
            SetStatus(result.Message);
        }
    }

    private void HideWindow() => AppWindow.Hide();

    private void SetStatus(string text)
    {
        if (_status != null) _status.Text = text;
    }

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
        AppWindow.Resize(new Windows.Graphics.SizeInt32(420, 480));

        nint hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        int ex = GetWindowLong(hwnd, GWL_EXSTYLE);
        SetWindowLong(hwnd, GWL_EXSTYLE, ex | WS_EX_TOOLWINDOW);
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

    private const int GWL_EXSTYLE = -20;
    private const int WS_EX_TOOLWINDOW = 0x00000080;

    [StructLayout(LayoutKind.Sequential)]
    private struct POINT { public int X; public int Y; }

    [DllImport("user32.dll")] private static extern bool GetCursorPos(out POINT lpPoint);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongW")] private static extern int GetWindowLong(nint hWnd, int nIndex);
    [DllImport("user32.dll", EntryPoint = "SetWindowLongW")] private static extern int SetWindowLong(nint hWnd, int nIndex, int dwNewLong);
}
