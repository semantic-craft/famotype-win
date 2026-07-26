using Famo.Settings.Core;
using Famo.Settings.Core.QuickPhrases;
using Famo.Settings.Theming;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace Famo.Settings.Views;

/// <summary>快捷短语（短）—— 本地编辑；拼音输入完整裸码触发，五笔走显式选择器。</summary>
public sealed class QuickPhrasesPage : UserControl
{
    private readonly QuickPhraseStore _store = new();
    private TextBox _code = null!;
    private TextBox _text = null!;
    private Button _save = null!;
    private StackPanel _list = null!;
    private TextBlock _status = null!;
    private string? _editingCode;

    public QuickPhrasesPage()
    {
        BuildContent();
    }

    private void BuildContent()
    {
        var sp = new StackPanel();
        sp.Children.Add(FamoUI.PaneHeader("快捷短语", "给固定话术设置短编码；拼音下输入完整编码，任意方案可用录制热键打开快捷短语面板。"));
        sp.Children.Add(FamoUI.Banner(true, "本地快捷短语只写入 Famo 托管的 famo_quick_send.txt，不改 custom_phrase.txt 或其他词库"));

        _status = new TextBlock
        {
            Text = "短语不能为空；编码会自动转成小写，需以字母开头，可含数字，最长 32 位。",
            FontSize = 12.5,
            Foreground = FamoUI.Br("Famo.Ink2"),
            Margin = new Thickness(0, 0, 0, 12),
            TextWrapping = TextWrapping.Wrap,
        };
        sp.Children.Add(_status);

        _code = new TextBox { PlaceholderText = "编码，如 fm 或 fmcs", MinWidth = 180 };
        _text = new TextBox
        {
            PlaceholderText = "短语文本",
            AcceptsReturn = false,
            TextWrapping = TextWrapping.Wrap,
            MinWidth = 320,
        };
        sp.Children.Add(FamoUI.Card("添加 / 编辑",
            FamoUI.Row("短语", "固定话术；不能包含换行或制表符。", _text, divider: false),
            FamoUI.Row("编码", "唯一键；输入完整编码时短语置顶，输入一半不会抢候选。", _code),
            FamoUI.RowFull(BuildActions(), divider: true)));

        _list = new StackPanel { Spacing = 8 };
        sp.Children.Add(FamoUI.Card("本地短语", FamoUI.RowFull(_list)));

        Content = sp;
        RenderList();
    }

    private FrameworkElement BuildActions()
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Margin = new Thickness(0, 12, 0, 0) };
        _save = new Button { Content = "添加短语" };
        _save.Click += (_, _) => SaveCurrent();
        var clear = new Button { Content = "清空" };
        clear.Click += (_, _) => ClearEditor();
        var picker = new Button { Content = "打开面板" };
        picker.Click += (_, _) => App.ShowQuickPhrasePicker();
        row.Children.Add(_save);
        row.Children.Add(clear);
        row.Children.Add(picker);
        return row;
    }

    private void SaveCurrent()
    {
        var entry = new QuickPhraseEntry
        {
            Code = _code.Text.Trim(),
            Text = _text.Text.Trim(),
        };
        string? error = QuickPhraseStore.Validate(entry);
        if (error != null)
        {
            SetStatus(error);
            return;
        }

        try
        {
            _store.SaveEdit(entry, _editingCode, FamoPaths.QuickSendTableFile);
        }
        catch (InvalidDataException ex)
        {
            SetStatus("未保存改动：" + ex.Message);
            return;
        }
        catch (Exception ex)
        {
            SetStatus("读取短语库失败，未保存改动，请重试：" + ex.Message);
            return;
        }
        ReloadResult result = App.SaveAndApplyDeploy();
        ClearEditor();
        RenderList();
        App.ReportReloadResult(
            result,
            _status,
            pending: "已保存，并已发送应用命令。",
            running: "正在应用快捷短语…",
            succeeded: "已保存，并已应用配置改动。",
            failedPrefix: "已保存到本地，但应用失败");
    }

    private void RenderList()
    {
        if (_list is null) return;
        _list.Children.Clear();
        IReadOnlyList<QuickPhraseEntry> entries = _store.Load();
        if (entries.Count == 0)
        {
            _list.Children.Add(new TextBlock
            {
                Text = "暂无快捷短语",
                FontSize = 13,
                Foreground = FamoUI.Br("Famo.Ink2"),
                Margin = new Thickness(0, 8, 0, 4),
            });
            return;
        }

        foreach (QuickPhraseEntry entry in entries)
        {
            _list.Children.Add(BuildEntry(entry));
        }
    }

    private UIElement BuildEntry(QuickPhraseEntry entry)
    {
        var info = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        info.Children.Add(new TextBlock
        {
            Text = entry.Text,
            FontSize = 13.5,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            Foreground = FamoUI.Br("Famo.Ink"),
            TextWrapping = TextWrapping.Wrap,
            MaxLines = 2,
        });
        info.Children.Add(new TextBlock
        {
            Text = entry.Code,
            FontFamily = FamoUI.Mono,
            FontSize = 12,
            Foreground = FamoUI.Br("Famo.Ink3"),
            TextWrapping = TextWrapping.NoWrap,
        });

        var edit = IconButton(Symbol.Edit, "编辑");
        edit.Click += (_, _) =>
        {
            _code.Text = entry.Code;
            _text.Text = entry.Text;
            _editingCode = entry.Code;
            UpdateSaveButton();
            SetStatus("正在编辑 " + entry.Code);
        };
        var delete = IconButton(Symbol.Delete, "删除");
        delete.Click += async (_, _) =>
        {
            bool confirmed = await FamoUI.Confirm(XamlRoot, "删除快捷短语",
                $"确定删除短语「{entry.Text}」（编码 {entry.Code}）吗？此操作无法撤销。");
            if (!confirmed) return;

            try
            {
                _store.Delete(entry.Code);
                _store.WriteTableDb(FamoPaths.QuickSendTableFile);
            }
            catch (Exception ex)
            {
                SetStatus("读取短语库失败，未保存改动，请重试：" + ex.Message);
                return;
            }
            ReloadResult result = App.SaveAndApplyDeploy();
            if (string.Equals(_editingCode, entry.Code, StringComparison.Ordinal))
                ClearEditor();
            RenderList();
            App.ReportReloadResult(
                result,
                _status,
                pending: "已删除，并已发送应用命令。",
                running: "正在应用快捷短语…",
                succeeded: "已删除，并已应用配置改动。",
                failedPrefix: "已删除本地记录，但应用失败");
        };

        var grid = new Grid
        {
            ColumnDefinitions =
            {
                new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) },
                new ColumnDefinition { Width = GridLength.Auto },
                new ColumnDefinition { Width = GridLength.Auto },
            },
            ColumnSpacing = 10,
        };
        Grid.SetColumn(info, 0);
        Grid.SetColumn(edit, 1);
        Grid.SetColumn(delete, 2);
        grid.Children.Add(info);
        grid.Children.Add(edit);
        grid.Children.Add(delete);

        return new Border
        {
            CornerRadius = new CornerRadius(7),
            Background = FamoUI.Br("Famo.Field"),
            BorderBrush = FamoUI.Br("Famo.Line2"),
            BorderThickness = new Thickness(1),
            Padding = new Thickness(10),
            Child = grid,
        };
    }

    private static Button IconButton(Symbol symbol, string tooltip)
    {
        var button = new Button
        {
            Content = new SymbolIcon(symbol),
            Width = 32,
            Height = 32,
            Padding = new Thickness(0),
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
            BorderThickness = new Thickness(0),
        };
        ToolTipService.SetToolTip(button, tooltip);
        return button;
    }

    private void ClearEditor()
    {
        _code.Text = "";
        _text.Text = "";
        _editingCode = null;
        UpdateSaveButton();
    }

    private void UpdateSaveButton()
    {
        if (_save != null) _save.Content = _editingCode == null ? "添加短语" : "保存修改";
    }

    private void SetStatus(string text)
    {
        if (_status != null) _status.Text = text;
    }
}
