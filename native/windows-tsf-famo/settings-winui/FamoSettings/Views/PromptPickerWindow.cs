using System.Runtime.InteropServices;
using Famo.Settings.Core.Insertion;
using Famo.Settings.Core.Prompts;
using Famo.Settings.Theming;
using Microsoft.UI.Text;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.System;

namespace Famo.Settings.Views;

/// <summary>提示词快速选择器：显式打开、变量填充后通过剪贴板粘贴到先前焦点窗口。</summary>
public sealed class PromptPickerWindow : Window
{
    private readonly PromptLibraryStore _store = new();
    private readonly ITextInsertionService _insertion;
    private PromptLibraryDocument _document = new();
    private TextBox _search = null!;
    private ComboBox _category = null!;
    private StackPanel _list = null!;
    private TextBlock _preview = null!;
    private TextBlock _status = null!;
    private bool _configured;
    private Grid _root = null!;

    public PromptPickerWindow(ITextInsertionService insertion)
    {
        _insertion = insertion;
        Title = "法墨提示词";
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
            Text = "提示词",
            FontSize = 18,
            FontWeight = FontWeights.SemiBold,
            Foreground = FamoUI.Br("Famo.Ink"),
        });
        _status = new TextBlock
        {
            Text = "搜索后回车插入；含 {{变量}} 的提示词会先填写变量。",
            FontSize = 12,
            Foreground = FamoUI.Br("Famo.Ink2"),
            TextWrapping = TextWrapping.Wrap,
        };
        header.Children.Add(_status);
        Grid.SetRow(header, 0);
        root.Children.Add(header);

        var controls = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Margin = new Thickness(0, 0, 0, 10) };
        _search = new TextBox { PlaceholderText = "搜索提示词", MinWidth = 260 };
        _search.TextChanged += (_, _) => RenderList();
        _search.KeyDown += SearchKeyDown;
        _category = new ComboBox { MinWidth = 130 };
        _category.SelectionChanged += (_, _) => RenderList();
        controls.Children.Add(_search);
        controls.Children.Add(_category);
        Grid.SetRow(controls, 1);
        root.Children.Add(controls);

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
            Text = "选择一个提示词查看预览。",
            FontSize = 12.5,
            Foreground = FamoUI.Br("Famo.Ink2"),
            TextWrapping = TextWrapping.Wrap,
            MaxLines = 4,
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
        _document = _store.Load();
        RefreshCategories();
        RenderList();
        Activate();
        if (!_configured) { ConfigurePresenter(); _configured = true; }
        PositionNearCursor();
        _search.Focus(FocusState.Programmatic);
    }

    private void RefreshCategories()
    {
        string? selected = SelectedTag(_category);
        _category.Items.Clear();
        _category.Items.Add(new ComboBoxItem { Content = "全部", Tag = "" });
        foreach (PromptCategory category in _document.Categories.Where(c => c.Enabled))
        {
            _category.Items.Add(new ComboBoxItem { Content = category.Name, Tag = category.Id });
        }
        SelectByTag(_category, selected ?? "");
    }

    private void RenderList()
    {
        if (_list is null) return;
        _list.Children.Clear();

        string? categoryId = SelectedTag(_category);
        IReadOnlyList<PromptLibraryEntry> prompts = PromptLibraryStore.FilterAndSort(
            _document.Prompts,
            _search?.Text,
            string.IsNullOrEmpty(categoryId) ? null : categoryId);

        if (prompts.Count == 0)
        {
            _list.Children.Add(new TextBlock
            {
                Text = "暂无匹配提示词",
                FontSize = 13,
                Foreground = FamoUI.Br("Famo.Ink2"),
                Margin = new Thickness(0, 8, 0, 0),
            });
            return;
        }

        foreach (PromptLibraryEntry prompt in prompts)
        {
            _list.Children.Add(BuildPromptRow(prompt));
        }
    }

    private UIElement BuildPromptRow(PromptLibraryEntry prompt)
    {
        var title = new TextBlock
        {
            Text = prompt.Pinned ? "★ " + prompt.Title : prompt.Title,
            FontSize = 13.5,
            FontWeight = FontWeights.SemiBold,
            Foreground = FamoUI.Br("Famo.Ink"),
            TextWrapping = TextWrapping.Wrap,
            MaxLines = 2,
        };
        var meta = new TextBlock
        {
            Text = Metadata(prompt),
            FontSize = 12,
            Foreground = FamoUI.Br("Famo.Ink3"),
            TextWrapping = TextWrapping.Wrap,
            MaxLines = 2,
        };
        var stack = new StackPanel { Spacing = 3 };
        stack.Children.Add(title);
        stack.Children.Add(meta);

        var item = new Border
        {
            CornerRadius = new CornerRadius(7),
            Background = FamoUI.Br("Famo.Field"),
            BorderBrush = FamoUI.Br("Famo.Line2"),
            BorderThickness = new Thickness(1),
            Padding = new Thickness(10),
            Child = stack,
            Tag = prompt,
        };
        item.PointerEntered += (_, _) =>
        {
            item.Background = FamoUI.Br("Famo.Hover");
            _preview.Text = Preview(prompt.Content);
        };
        item.PointerExited += (_, _) => item.Background = FamoUI.Br("Famo.Field");
        item.PointerPressed += async (_, _) => await InsertPromptAsync(prompt);
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
        PromptLibraryEntry? first = _list.Children
            .OfType<Border>()
            .Select(border => border.Tag as PromptLibraryEntry)
            .FirstOrDefault(prompt => prompt is not null);
        if (first is null) return;
        e.Handled = true;
        await InsertPromptAsync(first);
    }

    private async Task InsertPromptAsync(PromptLibraryEntry prompt)
    {
        Dictionary<string, string>? values = await CollectVariablesAsync(prompt);
        if (values is null) return;

        string rendered = PromptRenderer.Render(prompt.Content, values);
        HideWindow();
        TextInsertionResult result = await _insertion.InsertAsync(rendered, CancellationToken.None);
        if (!result.Success)
        {
            Activate();
            SetStatus(result.Message);
        }
    }

    private async Task<Dictionary<string, string>?> CollectVariablesAsync(PromptLibraryEntry prompt)
    {
        IReadOnlyList<PromptVariable> variables = PromptVariableParser.Extract(prompt.Content);
        if (variables.Count == 0) return new Dictionary<string, string>();

        var fields = new Dictionary<string, TextBox>(StringComparer.Ordinal);
        var panel = new StackPanel { Spacing = 8 };
        foreach (PromptVariable variable in variables)
        {
            panel.Children.Add(new TextBlock
            {
                Text = variable.Name,
                FontSize = 12.5,
                Foreground = FamoUI.Br("Famo.Ink"),
            });
            var box = new TextBox
            {
                PlaceholderText = variable.DefaultValue ?? variable.Name,
                MinWidth = 320,
            };
            fields[variable.Name] = box;
            panel.Children.Add(box);
        }

        var dialog = new ContentDialog
        {
            Title = "填写变量",
            Content = panel,
            PrimaryButtonText = "插入",
            CloseButtonText = "取消",
            XamlRoot = Content.XamlRoot,
        };
        ContentDialogResult result = await dialog.ShowAsync();
        if (result != ContentDialogResult.Primary) return null;

        return fields
            .Where(pair => !string.IsNullOrWhiteSpace(pair.Value.Text))
            .ToDictionary(pair => pair.Key, pair => pair.Value.Text, StringComparer.Ordinal);
    }

    private string Metadata(PromptLibraryEntry prompt)
    {
        string category = _document.Categories.FirstOrDefault(c => c.Id == prompt.CategoryId)?.Name ?? prompt.CategoryId;
        string tags = prompt.Tags.Count == 0 ? "无标签" : string.Join(" / ", prompt.Tags);
        IReadOnlyList<PromptVariable> variables = PromptVariableParser.Extract(prompt.Content);
        string variableText = variables.Count == 0 ? "无变量" : variables.Count + " 个变量";
        return $"{category} · {tags} · {variableText}";
    }

    private static string Preview(string text)
    {
        string oneLine = text.Replace("\r\n", " ").Replace('\n', ' ').Replace('\r', ' ');
        return oneLine.Length <= 220 ? oneLine : oneLine[..220] + "...";
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
        AppWindow.Resize(new Windows.Graphics.SizeInt32(460, 520));

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

    private static string? SelectedTag(ComboBox? combo)
    {
        if (combo?.SelectedItem is ComboBoxItem item)
        {
            return item.Tag as string;
        }
        return null;
    }

    private static void SelectByTag(ComboBox combo, string tag)
    {
        foreach (object? item in combo.Items)
        {
            if (item is ComboBoxItem comboItem && string.Equals(comboItem.Tag as string, tag, StringComparison.Ordinal))
            {
                combo.SelectedItem = comboItem;
                return;
            }
        }

        if (combo.Items.Count > 0) combo.SelectedIndex = 0;
    }

    private const int GWL_EXSTYLE = -20;
    private const int WS_EX_TOOLWINDOW = 0x00000080;

    [StructLayout(LayoutKind.Sequential)]
    private struct POINT { public int X; public int Y; }

    [DllImport("user32.dll")] private static extern bool GetCursorPos(out POINT lpPoint);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongW")] private static extern int GetWindowLong(nint hWnd, int nIndex);
    [DllImport("user32.dll", EntryPoint = "SetWindowLongW")] private static extern int SetWindowLong(nint hWnd, int nIndex, int dwNewLong);
}
