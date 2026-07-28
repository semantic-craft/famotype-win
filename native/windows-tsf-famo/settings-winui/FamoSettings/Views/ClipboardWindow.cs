using System.Runtime.InteropServices;
using Famo.Settings.Core;
using Famo.Settings.Core.Clipboard;
using Famo.Settings.Interop;
using Famo.Settings.Theming;
using Microsoft.UI.Text;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace Famo.Settings.Views;

/// <summary>剪贴板历史浮窗：状态栏触发，不抢焦点，点选后 SendInput 回插到原焦点。</summary>
public sealed class ClipboardWindow : Window
{
    private readonly ClipboardHistoryStore _store = new();
    private StackPanel _list = null!;
    private TextBlock _status = null!;
    private bool _configured;

    public ClipboardWindow()
    {
        Title = "法墨剪贴板历史";
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
            },
        };

        var header = new StackPanel { Margin = new Thickness(0, 0, 0, 10) };
        header.Children.Add(new TextBlock
        {
            Text = "剪贴板历史",
            FontSize = 18,
            FontWeight = FontWeights.SemiBold,
            Foreground = FamoUI.Br("Famo.Ink"),
        });
        _status = new TextBlock
        {
            Text = "点击历史项可直接上屏；捕获仅在启用后生效。",
            FontSize = 12,
            Foreground = FamoUI.Br("Famo.Ink2"),
            TextWrapping = TextWrapping.Wrap,
        };
        header.Children.Add(_status);
        Grid.SetRow(header, 0);
        root.Children.Add(header);

        FrameworkElement actions = BuildActions();
        Grid.SetRow(actions, 1);
        root.Children.Add(actions);

        _list = new StackPanel { Spacing = 8 };
        var scroller = new ScrollViewer
        {
            Content = _list,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
        };
        Grid.SetRow(scroller, 2);
        root.Children.Add(scroller);

        Content = root;
        FamoTheme.Changed += () => { try { root.Background = FamoUI.Br("Famo.Card"); RenderList(); } catch { } };
        RenderList();
    }

    private FrameworkElement BuildActions()
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Margin = new Thickness(0, 0, 0, 10) };
        var capture = new Button { Content = "捕获当前剪贴板" };
        capture.Click += async (_, _) => await CaptureCurrentClipboardAsync();
        var clear = new Button { Content = "清空记录" };
        clear.Click += (_, _) =>
        {
            _store.Clear();
            RenderList();
            SetStatus("剪贴板历史已清空。");
        };
        row.Children.Add(capture);
        row.Children.Add(clear);
        return row;
    }

    public void ShowNearCursor()
    {
        RenderList();
        this.Activate();
        if (!_configured) { ConfigurePresenter(); _configured = true; }
        PositionNearCursor();
        ApplyNoActivate();
    }

    private async Task CaptureCurrentClipboardAsync()
    {
        string? text = await ClipboardReader.ReadTextAsync();
        try
        {
            if (_store.AddText(text, App.Settings.Clipboard.Enabled))
            {
                RenderList();
                SetStatus("已捕获当前剪贴板文本。");
                return;
            }

            SetStatus(App.Settings.Clipboard.Enabled ? "当前剪贴板没有可捕获的文本。" : "剪贴板历史未启用，未捕获。");
        }
        catch (Exception ex)
        {
            SetStatus("捕获剪贴板失败：" + ex.Message);
        }
    }

    private void RenderList()
    {
        if (_list is null) return;
        _list.Children.Clear();
        IReadOnlyList<ClipboardHistoryEntry> entries = _store.Load();
        if (entries.Count == 0)
        {
            _list.Children.Add(new TextBlock
            {
                Text = "暂无历史",
                FontSize = 13,
                Foreground = FamoUI.Br("Famo.Ink2"),
                Margin = new Thickness(0, 8, 0, 0),
            });
            return;
        }

        foreach (ClipboardHistoryEntry entry in entries)
        {
            _list.Children.Add(BuildEntry(entry));
        }
    }

    private UIElement BuildEntry(ClipboardHistoryEntry entry)
    {
        var text = new TextBlock
        {
            Text = Preview(entry.Text),
            FontSize = 13.5,
            Foreground = FamoUI.Br("Famo.Ink"),
            TextWrapping = TextWrapping.Wrap,
            MaxLines = 3,
        };
        var time = new TextBlock
        {
            Text = entry.CreatedAt.ToLocalTime().ToString("HH:mm:ss"),
            FontSize = 12,
            Foreground = FamoUI.Br("Famo.Ink3"),
            FontFamily = FamoUI.Mono,
        };
        var stack = new StackPanel { Spacing = 4 };
        stack.Children.Add(text);
        stack.Children.Add(time);

        var item = new Border
        {
            CornerRadius = new CornerRadius(7),
            Background = FamoUI.Br("Famo.Field"),
            BorderBrush = FamoUI.Br("Famo.Line2"),
            BorderThickness = new Thickness(1),
            Padding = new Thickness(10),
            Child = stack,
        };
        item.PointerEntered += (_, _) => item.Background = FamoUI.Br("Famo.Hover");
        item.PointerExited += (_, _) => item.Background = FamoUI.Br("Famo.Field");
        item.PointerPressed += (_, _) =>
        {
            HideWindow();
            TextInjector.Inject(entry.Text);
        };
        return item;
    }

    private void HideWindow() => AppWindow.Hide();

    private void SetStatus(string text)
    {
        if (_status != null) _status.Text = text;
    }

    private static string Preview(string text)
    {
        string oneLine = text.Replace("\r\n", " ").Replace('\n', ' ').Replace('\r', ' ');
        return oneLine.Length <= 120
            ? oneLine
            : TextElementTruncator.Truncate(oneLine, 120) + "...";
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
        AppWindow.Resize(new Windows.Graphics.SizeInt32(420, 430));

        nint hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        int ex = GetWindowLong(hwnd, GWL_EXSTYLE);
        SetWindowLong(hwnd, GWL_EXSTYLE, ex | WS_EX_TOOLWINDOW);
    }

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

    private const int GWL_EXSTYLE = -20;
    private const int WS_EX_NOACTIVATE = 0x08000000;
    private const int WS_EX_TOOLWINDOW = 0x00000080;

    [StructLayout(LayoutKind.Sequential)]
    private struct POINT { public int X; public int Y; }

    [DllImport("user32.dll")] private static extern bool GetCursorPos(out POINT lpPoint);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongW")] private static extern int GetWindowLong(nint hWnd, int nIndex);
    [DllImport("user32.dll", EntryPoint = "SetWindowLongW")] private static extern int SetWindowLong(nint hWnd, int nIndex, int dwNewLong);
}
