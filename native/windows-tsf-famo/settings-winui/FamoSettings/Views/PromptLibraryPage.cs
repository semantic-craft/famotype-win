using System.Text.Json;
using Famo.Settings.Core;
using Famo.Settings.Core.Prompts;
using Famo.Settings.Theming;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using Windows.Storage;
using Windows.Storage.Pickers;

namespace Famo.Settings.Views;

/// <summary>提示词库（提）—— 本地长提示词管理，不进入 Rime 输入热路径。</summary>
public sealed class PromptLibraryPage : UserControl
{
    private readonly PromptLibraryStore _store = new();
    private PromptLibraryDocument _document = new();
    private TextBox _search = null!;
    private ComboBox _filterCategory = null!;
    private StackPanel _list = null!;
    private TextBlock _status = null!;
    private TextBox _title = null!;
    private TextBox _content = null!;
    private TextBox _tags = null!;
    private TextBox _trigger = null!;
    private TextBox _notes = null!;
    private TextBox _sourceUrl = null!;
    private ComboBox _editorCategory = null!;
    private ToggleButton _enabled = null!;
    private ToggleButton _pinned = null!;
    private TextBlock _variables = null!;
    private Button _save = null!;
    private TextBox _categoryName = null!;
    private StackPanel _categoryList = null!;
    private string? _editingPromptId;
    private string? _editingCategoryId;

    public PromptLibraryPage()
    {
        LoadDocument();
        BuildContent();
        PrefillPendingSelection();
    }

    private void BuildContent()
    {
        var sp = new StackPanel();
        sp.Children.Add(FamoUI.PaneHeader("提示词库", "管理可复用的长提示词；本地保存、显式插入，不进入 Rime 热路径。"));
        sp.Children.Add(FamoUI.Banner(false, "提示词只保存在本机 Famo 目录；不自动同步、不上传、不触发 AI 请求"));

        _status = new TextBlock
        {
            Text = "提示词支持多行内容和 {{变量}}；快捷短语仍在「短」页管理。",
            FontSize = 12.5,
            Foreground = FamoUI.Br("Famo.Ink2"),
            Margin = new Thickness(0, 0, 0, 12),
            TextWrapping = TextWrapping.Wrap,
        };
        sp.Children.Add(_status);

        _search = new TextBox { PlaceholderText = "搜索标题、内容、标签、分类或触发词", MinWidth = 240 };
        _search.TextChanged += (_, _) => RenderList();
        _filterCategory = new ComboBox { MinWidth = 150 };
        _filterCategory.SelectionChanged += (_, _) => RenderList();
        sp.Children.Add(FamoUI.Card("检索",
            FamoUI.Row("搜索", "筛选本地提示词，不读取外部应用内容。", _search, divider: false),
            FamoUI.Row("分类", "按分类缩小范围。", _filterCategory),
            FamoUI.RowFull(BuildImportExportActions(), divider: true)));

        _list = new StackPanel { Spacing = 8 };
        sp.Children.Add(FamoUI.Card("本地提示词", FamoUI.RowFull(_list)));

        sp.Children.Add(BuildEditorCard());
        sp.Children.Add(BuildCategoryCard());

        Content = sp;
        RefreshCategoryControls();
        RenderList();
        UpdateVariablePreview();
    }

    private Border BuildEditorCard()
    {
        _title = new TextBox { PlaceholderText = "标题", MinWidth = 260 };
        _editorCategory = new ComboBox { MinWidth = 180 };
        _tags = new TextBox { PlaceholderText = "标签，用逗号分隔", MinWidth = 260 };
        _trigger = new TextBox { PlaceholderText = "可选触发词，如 ;;case", MinWidth = 220 };
        _content = new TextBox
        {
            PlaceholderText = "提示词内容，可使用 {{变量}}",
            AcceptsReturn = true,
            TextWrapping = TextWrapping.Wrap,
            MinHeight = 160,
            MinWidth = 360,
        };
        _content.TextChanged += (_, _) => UpdateVariablePreview();
        _notes = new TextBox { PlaceholderText = "备注（本地）", MinWidth = 260 };
        _sourceUrl = new TextBox { PlaceholderText = "来源 URL（可选）", MinWidth = 260 };
        _enabled = FamoUI.Check(true, _ => { });
        _pinned = FamoUI.Check(false, _ => { });
        _variables = new TextBlock
        {
            Text = "变量：无",
            FontSize = 12.5,
            Foreground = FamoUI.Br("Famo.Ink2"),
            TextWrapping = TextWrapping.Wrap,
        };

        return FamoUI.Card("添加 / 编辑",
            FamoUI.Row("标题", "留空时使用内容第一行。", _title, divider: false),
            FamoUI.Row("分类", "提示词所属分类。", _editorCategory),
            FamoUI.Row("标签", "用于搜索和分组。", _tags),
            FamoUI.Row("触发词", "供后续 picker 或 typed trigger 使用；首版不监听键入触发。", _trigger),
            FamoUI.Row("启用", "关闭后不出现在快速选择器。", _enabled),
            FamoUI.Row("置顶", "置顶提示词在列表中优先显示。", _pinned),
            FamoUI.RowFull(_content, divider: true),
            FamoUI.Row("备注", "仅保存在本机。", _notes),
            FamoUI.Row("来源", "可记录提示词来源，不会自动访问。", _sourceUrl),
            FamoUI.RowFull(_variables, divider: true),
            FamoUI.RowFull(BuildEditorActions(), divider: true));
    }

    private Border BuildCategoryCard()
    {
        _categoryName = new TextBox { PlaceholderText = "分类名称", MinWidth = 220 };
        _categoryList = new StackPanel { Spacing = 8 };
        return FamoUI.Card("分类",
            FamoUI.Row("名称", "新增或重命名本地分类。", _categoryName, divider: false),
            FamoUI.RowFull(BuildCategoryActions(), divider: true),
            FamoUI.RowFull(_categoryList, divider: true));
    }

    private FrameworkElement BuildImportExportActions()
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Margin = new Thickness(0, 12, 0, 0) };
        var import = new Button { Content = "导入 JSON" };
        import.Click += async (_, _) => await ImportAsync();
        var export = new Button { Content = "导出 JSON" };
        export.Click += async (_, _) => await ExportAsync();
        var picker = new Button { Content = "快速插入" };
        picker.Click += (_, _) => App.ShowPromptPicker();
        var saveSelection = new Button { Content = "保存选中为提示词" };
        saveSelection.Click += (_, _) => App.ShowPromptSaveSelection();
        row.Children.Add(import);
        row.Children.Add(export);
        row.Children.Add(picker);
        row.Children.Add(saveSelection);
        return row;
    }

    private FrameworkElement BuildEditorActions()
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Margin = new Thickness(0, 12, 0, 0) };
        _save = new Button { Content = "添加提示词" };
        _save.Click += (_, _) => SavePrompt();
        var clear = new Button { Content = "清空" };
        clear.Click += (_, _) => ClearEditor();
        row.Children.Add(_save);
        row.Children.Add(clear);
        return row;
    }

    private FrameworkElement BuildCategoryActions()
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Margin = new Thickness(0, 12, 0, 0) };
        var save = new Button { Content = "保存分类" };
        save.Click += (_, _) => SaveCategory();
        var clear = new Button { Content = "清空" };
        clear.Click += (_, _) => ClearCategoryEditor();
        row.Children.Add(save);
        row.Children.Add(clear);
        return row;
    }

    private void LoadDocument()
    {
        _document = _store.Load();
    }

    private void RefreshAll()
    {
        LoadDocument();
        RefreshCategoryControls();
        RenderList();
        RenderCategories();
    }

    private void RefreshCategoryControls()
    {
        string? selectedFilter = SelectedTag(_filterCategory);
        string? selectedEditor = SelectedTag(_editorCategory) ?? PromptLibraryStore.DefaultCategoryId;

        _filterCategory.Items.Clear();
        _filterCategory.Items.Add(new ComboBoxItem { Content = "全部", Tag = "" });
        _editorCategory.Items.Clear();

        foreach (PromptCategory category in _document.Categories.Where(c => c.Enabled))
        {
            _filterCategory.Items.Add(new ComboBoxItem { Content = category.Name, Tag = category.Id });
            _editorCategory.Items.Add(new ComboBoxItem { Content = category.Name, Tag = category.Id });
        }

        SelectByTag(_filterCategory, selectedFilter ?? "");
        SelectByTag(_editorCategory, selectedEditor);
        RenderCategories();
    }

    private void RenderList()
    {
        if (_list is null) return;
        _list.Children.Clear();

        string? categoryId = SelectedTag(_filterCategory);
        IReadOnlyList<PromptLibraryEntry> prompts = PromptLibraryStore.FilterAndSort(
            _document.Prompts,
            _search?.Text,
            string.IsNullOrEmpty(categoryId) ? null : categoryId,
            includeDisabled: true);

        if (prompts.Count == 0)
        {
            _list.Children.Add(new TextBlock
            {
                Text = "暂无匹配提示词",
                FontSize = 13,
                Foreground = FamoUI.Br("Famo.Ink2"),
                Margin = new Thickness(0, 8, 0, 4),
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
        var info = new StackPanel { Spacing = 4, VerticalAlignment = VerticalAlignment.Center };
        info.Children.Add(new TextBlock
        {
            Text = prompt.Pinned ? "★ " + prompt.Title : prompt.Title,
            FontSize = 13.5,
            FontWeight = FontWeights.SemiBold,
            Foreground = FamoUI.Br("Famo.Ink"),
            TextWrapping = TextWrapping.Wrap,
            MaxLines = 2,
        });
        info.Children.Add(new TextBlock
        {
            Text = Metadata(prompt),
            FontSize = 12,
            Foreground = FamoUI.Br("Famo.Ink3"),
            TextWrapping = TextWrapping.Wrap,
            MaxLines = 2,
        });
        info.Children.Add(new TextBlock
        {
            Text = Preview(prompt.Content),
            FontSize = 12.5,
            Foreground = FamoUI.Br("Famo.Ink2"),
            TextWrapping = TextWrapping.Wrap,
            MaxLines = 3,
        });

        var edit = IconButton(Symbol.Edit, "编辑");
        edit.Click += (_, _) => EditPrompt(prompt);
        var delete = IconButton(Symbol.Delete, "删除");
        delete.Click += async (_, _) =>
        {
            bool confirmed = await FamoUI.Confirm(XamlRoot, "删除提示词",
                $"确定删除提示词「{prompt.Title}」吗？此操作无法撤销。");
            if (!confirmed) return;

            try
            {
                _store.DeletePrompt(prompt.Id);
                if (_editingPromptId == prompt.Id) ClearEditor();
                RefreshAll();
                SetStatus("已删除提示词。");
            }
            catch (Exception ex)
            {
                SetStatus("删除提示词失败：" + ex.Message);
            }
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

    private void RenderCategories()
    {
        if (_categoryList is null) return;
        _categoryList.Children.Clear();

        foreach (PromptCategory category in _document.Categories)
        {
            _categoryList.Children.Add(BuildCategoryRow(category));
        }
    }

    private UIElement BuildCategoryRow(PromptCategory category)
    {
        int count = _document.Prompts.Count(p => p.CategoryId == category.Id);
        var label = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        label.Children.Add(new TextBlock
        {
            Text = category.Name,
            FontSize = 13.5,
            FontWeight = FontWeights.SemiBold,
            Foreground = FamoUI.Br("Famo.Ink"),
        });
        label.Children.Add(new TextBlock
        {
            Text = $"{category.Id} · {count} 条",
            FontSize = 12,
            FontFamily = FamoUI.Mono,
            Foreground = FamoUI.Br("Famo.Ink3"),
        });

        var edit = IconButton(Symbol.Edit, "编辑分类");
        edit.Click += (_, _) =>
        {
            _editingCategoryId = category.Id;
            _categoryName.Text = category.Name;
            SetStatus("正在编辑分类 " + category.Name);
        };
        var delete = IconButton(Symbol.Delete, "删除分类");
        delete.IsEnabled = category.Id != PromptLibraryStore.DefaultCategoryId;
        delete.Click += async (_, _) =>
        {
            bool confirmed = await FamoUI.Confirm(XamlRoot, "删除分类",
                $"确定删除分类「{category.Name}」吗？原分类下的提示词会移到默认分类，此操作无法撤销。");
            if (!confirmed) return;

            try
            {
                if (_store.DeleteCategory(category.Id))
                {
                    if (_editingCategoryId == category.Id) ClearCategoryEditor();
                    RefreshAll();
                    SetStatus("已删除分类，原提示词已移到默认分类。");
                }
            }
            catch (Exception ex)
            {
                SetStatus("删除分类失败：" + ex.Message);
            }
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
        Grid.SetColumn(label, 0);
        Grid.SetColumn(edit, 1);
        Grid.SetColumn(delete, 2);
        grid.Children.Add(label);
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

    private void SavePrompt()
    {
        try
        {
            _store.UpsertPrompt(new PromptLibraryEntry
            {
                Id = _editingPromptId ?? string.Empty,
                Title = _title.Text,
                Content = _content.Text,
                CategoryId = SelectedTag(_editorCategory) ?? PromptLibraryStore.DefaultCategoryId,
                Tags = SplitTags(_tags.Text),
                Trigger = _trigger.Text,
                Notes = _notes.Text,
                SourceUrl = _sourceUrl.Text,
                Enabled = _enabled.IsChecked == true,
                Pinned = _pinned.IsChecked == true,
            });
            ClearEditor();
            RefreshAll();
            SetStatus("已保存提示词。");
        }
        catch (InvalidDataException ex)
        {
            SetStatus(ex.Message);
        }
        catch (Exception ex)
        {
            SetStatus("保存提示词失败：" + ex.Message);
        }
    }

    private void EditPrompt(PromptLibraryEntry prompt)
    {
        _editingPromptId = prompt.Id;
        _title.Text = prompt.Title;
        _content.Text = prompt.Content;
        _tags.Text = string.Join(", ", prompt.Tags);
        _trigger.Text = prompt.Trigger ?? "";
        _notes.Text = prompt.Notes ?? "";
        _sourceUrl.Text = prompt.SourceUrl ?? "";
        _enabled.IsChecked = prompt.Enabled;
        _pinned.IsChecked = prompt.Pinned;
        SelectByTag(_editorCategory, prompt.CategoryId);
        _save.Content = "保存修改";
        UpdateVariablePreview();
        SetStatus("正在编辑提示词 " + prompt.Title);
    }

    private void ClearEditor()
    {
        _editingPromptId = null;
        _title.Text = "";
        _content.Text = "";
        _tags.Text = "";
        _trigger.Text = "";
        _notes.Text = "";
        _sourceUrl.Text = "";
        _enabled.IsChecked = true;
        _pinned.IsChecked = false;
        SelectByTag(_editorCategory, PromptLibraryStore.DefaultCategoryId);
        _save.Content = "添加提示词";
        UpdateVariablePreview();
    }

    private void PrefillPendingSelection()
    {
        string? content = App.TakePendingPromptContent();
        string? status = App.TakePendingPromptStatus();
        if (!string.IsNullOrWhiteSpace(content))
        {
            _content.Text = content;
            _title.Text = SuggestedTitle(content);
            _enabled.IsChecked = true;
            _pinned.IsChecked = false;
            UpdateVariablePreview();
        }
        if (!string.IsNullOrWhiteSpace(status))
        {
            SetStatus(status);
        }
    }

    private void SaveCategory()
    {
        try
        {
            string name = _categoryName.Text.Trim();
            if (string.IsNullOrEmpty(name)) throw new InvalidDataException("分类名称不能为空");
            _store.UpsertCategory(new PromptCategory
            {
                Id = _editingCategoryId ?? MakeCategoryId(name),
                Name = name,
                Enabled = true,
            });
            ClearCategoryEditor();
            RefreshAll();
            SetStatus("已保存分类。");
        }
        catch (InvalidDataException ex)
        {
            SetStatus(ex.Message);
        }
        catch (Exception ex)
        {
            SetStatus("保存分类失败：" + ex.Message);
        }
    }

    private void ClearCategoryEditor()
    {
        _editingCategoryId = null;
        _categoryName.Text = "";
    }

    private async Task ImportAsync()
    {
        try
        {
            FileOpenPicker picker = new();
            WinRT.Interop.InitializeWithWindow.Initialize(picker, WindowHandle());
            picker.FileTypeFilter.Add(".json");
            StorageFile? file = await picker.PickSingleFileAsync();
            if (file is null) return;

            string json = await FileIO.ReadTextAsync(file);
            PromptLibraryImportResult result = _store.ImportJson(json);
            RefreshAll();
            SetStatus($"导入完成：{result.ImportedPrompts} 条提示词，跳过 {result.SkippedPrompts} 条。");
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException or InvalidDataException)
        {
            SetStatus("导入失败：" + ex.Message);
        }
    }

    private async Task ExportAsync()
    {
        try
        {
            FileSavePicker picker = new();
            WinRT.Interop.InitializeWithWindow.Initialize(picker, WindowHandle());
            picker.SuggestedFileName = "famo-prompt-library";
            picker.FileTypeChoices.Add("JSON", new List<string> { ".json" });
            StorageFile? file = await picker.PickSaveFileAsync();
            if (file is null) return;

            await FileIO.WriteTextAsync(file, _store.ExportJson());
            SetStatus("已导出提示词库 JSON。");
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            SetStatus("导出失败：" + ex.Message);
        }
    }

    private void UpdateVariablePreview()
    {
        if (_variables is null) return;
        string content = _content?.Text ?? string.Empty;
        IReadOnlyList<PromptVariable> variables = PromptVariableParser.Extract(content);
        string text = variables.Count == 0
            ? "变量：无"
            : "变量：" + string.Join("、", variables.Select(v => v.Name));

        int rawPairs = Math.Min(
            (content.Length - content.Replace("{{", "").Length) / 2,
            (content.Length - content.Replace("}}", "").Length) / 2);
        int unrecognized = rawPairs - variables.Count;
        if (unrecognized > 0)
        {
            text += $"；另有 {unrecognized} 处疑似未识别的 {{{{}}}} 语法";
        }
        _variables.Text = text;
    }

    private static IntPtr WindowHandle()
    {
        if (App.Window is null) throw new InvalidOperationException("设置窗口尚未就绪");
        return WinRT.Interop.WindowNative.GetWindowHandle(App.Window);
    }

    private string Metadata(PromptLibraryEntry prompt)
    {
        string category = _document.Categories.FirstOrDefault(c => c.Id == prompt.CategoryId)?.Name ?? prompt.CategoryId;
        string tags = prompt.Tags.Count == 0 ? "无标签" : string.Join(" / ", prompt.Tags);
        string trigger = string.IsNullOrWhiteSpace(prompt.Trigger) ? "" : " · " + prompt.Trigger;
        string enabled = prompt.Enabled ? "" : " · 已停用";
        return $"{category} · {tags}{trigger}{enabled}";
    }

    private static string Preview(string text)
    {
        string oneLine = text.Replace("\r\n", " ").Replace('\n', ' ').Replace('\r', ' ');
        return oneLine.Length <= 180
            ? oneLine
            : TextElementTruncator.Truncate(oneLine, 180) + "...";
    }

    private static List<string> SplitTags(string text) =>
        text.Split(new[] { ',', '，', ';', '；' }, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Where(tag => tag.Length > 0)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToList();

    private static string MakeCategoryId(string name)
    {
        string clean = new string(name.Trim().ToLowerInvariant()
            .Select(ch => char.IsWhiteSpace(ch) ? '-' : ch)
            .Where(ch => char.IsLetterOrDigit(ch) || ch == '-' || ch == '_')
            .ToArray())
            .Trim('-');
        return string.IsNullOrEmpty(clean) ? Guid.NewGuid().ToString("N") : clean;
    }

    private static string SuggestedTitle(string content)
    {
        string title = content
            .Split(new[] { "\r\n", "\n", "\r" }, StringSplitOptions.None)
            .Select(line => line.Trim())
            .FirstOrDefault(line => line.Length > 0) ?? "选中文本提示词";
        return TextElementTruncator.Truncate(title, PromptLibraryStore.MaxGeneratedTitleLength);
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

    private void SetStatus(string text)
    {
        if (_status != null) _status.Text = text;
    }
}
