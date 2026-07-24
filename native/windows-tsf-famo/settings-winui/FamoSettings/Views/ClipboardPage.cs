using Famo.Settings.Core.Clipboard;
using Famo.Settings.Interop;
using Famo.Settings.Theming;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;

namespace Famo.Settings.Views;

/// <summary>剪贴板设置页：纯本地、默认关闭、按需捕获。</summary>
public sealed class ClipboardPage : UserControl
{
    private readonly ClipboardHistoryStore _store = new();
    private StackPanel _list = null!;
    private TextBlock _status = null!;

    public ClipboardPage()
    {
        BuildContent();
    }

    private void BuildContent()
    {
        var sp = new StackPanel();
        sp.Children.Add(FamoUI.PaneHeader("剪贴板", "记录最近复制的文本，按需捕获，本地保存。"));
        sp.Children.Add(FamoUI.Banner(false, "纯本地，不同步、不上传、不进入任何 AI 或网络请求"));

        _status = new TextBlock
        {
            Text = App.Settings.Clipboard.Enabled ? "已启用，点击捕获按钮才会记录当前剪贴板。" : "默认关闭；开启后仍只会按需捕获。",
            FontSize = 12.5,
            Foreground = FamoUI.Br("Famo.Ink2"),
            Margin = new Thickness(0, 0, 0, 12),
            TextWrapping = TextWrapping.Wrap,
        };
        sp.Children.Add(_status);

        sp.Children.Add(FamoUI.Card("本地记录",
            FamoUI.Row("启用剪贴板面板", "关闭时不会捕获，也不会写入 clipboard-history.json。",
                FamoUI.Pill(App.Settings.Clipboard.Enabled, v =>
                {
                    App.Settings.Clipboard.Enabled = v;
                    App.Store.Save(App.Settings);
                    SetStatus(v ? "剪贴板历史已启用。" : "剪贴板历史已关闭；捕获按钮不会写入。");
                    RenderList();
                }), divider: false),
            FamoUI.RowFull(BuildActions(), divider: true)));

        _list = new StackPanel { Spacing = 8 };
        sp.Children.Add(FamoUI.Card("本地历史", FamoUI.RowFull(_list)));

        Content = sp;
        RenderList();
    }

    private FrameworkElement BuildActions()
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Margin = new Thickness(0, 12, 0, 0) };
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
                Foreground = FamoUI.Br("Famo.Ink2"),
                FontSize = 13,
                Margin = new Thickness(0, 8, 0, 4),
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
            Text = entry.CreatedAt.ToLocalTime().ToString("yyyy-MM-dd HH:mm"),
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
            BorderBrush = FamoUI.Br("Famo.Line2"),
            BorderThickness = new Thickness(1),
            Background = FamoUI.Br("Famo.Field"),
            Padding = new Thickness(10),
            Child = stack,
        };
        item.PointerEntered += (_, _) => item.Background = FamoUI.Br("Famo.Hover");
        item.PointerExited += (_, _) => item.Background = FamoUI.Br("Famo.Field");
        item.PointerPressed += (_, _) =>
        {
            TextInjector.Inject(entry.Text);
            SetStatus("已将选中的剪贴板历史上屏。");
        };
        return item;
    }

    private void SetStatus(string text)
    {
        if (_status != null) _status.Text = text;
    }

    private static string Preview(string text)
    {
        string oneLine = text.Replace("\r\n", " ").Replace('\n', ' ').Replace('\r', ' ');
        return oneLine.Length <= 140 ? oneLine : oneLine[..140] + "...";
    }
}
