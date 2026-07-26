using Famo.Settings.Core.Ai;
using Famo.Settings.Core.Insertion;
using Famo.Settings.Theming;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.ApplicationModel.DataTransfer;

namespace Famo.Settings.Views;

/// <summary>划词工具箱；上方技能进入各自窗口，下方任意提问是兜底。</summary>
public sealed class AiConversationWindow : Window
{
    private readonly AiProviderProfileStore _providerStore = new();
    private readonly ISecretStore _secretStore = new WindowsCredentialSecretStore();
    private readonly string? _selectedText;
    private readonly ITextInsertionService? _replacement;
    /// <summary>已完成的问答轮，按序喂回后续请求（真·多轮）；client 侧自会截到历史上限。</summary>
    private readonly List<AiChatTurn> _turns = new();

    private TextBox _prompt = null!;
    private TextBox _result = null!;
    private TextBlock _status = null!;
    private Button _send = null!;

    public AiConversationWindow(string? selectedText = null, ITextInsertionService? replacement = null)
    {
        _selectedText = selectedText;
        _replacement = replacement;
        Title = selectedText is null ? "任意提问" : "划词工具箱";
        BuildContent();
        Activated += (_, _) =>
        {
            if (_prompt.IsEnabled)
                _prompt.DispatcherQueue.TryEnqueue(() => _prompt.Focus(FocusState.Programmatic));
        };
    }

    private void BuildContent()
    {
        bool askEnabled = _selectedText is null || App.Settings.Ai.AskAnythingSkillEnabled;
        var root = new ScrollViewer
        {
            Padding = new Thickness(24),
            Background = FamoUI.Br("Famo.Bg"),
        };
        var sp = new StackPanel();
        sp.Children.Add(FamoUI.PaneHeader(_selectedText is null ? "任意提问" : "划词工具箱", _selectedText is null
            ? "只处理你在这里主动发送的文本；不会读取普通输入候选。"
            : "翻译和辅助检索会留在本窗口，其他技能使用各自的确认窗口。"));

        _status = new TextBlock
        {
            Text = _selectedText is null
                ? "输入问题后发送；结果只显示在本窗口，可手动复制。"
                : "已读取当前选区。上方是启用的技能，下方任意提问作为兜底。",
            FontSize = 12.5,
            Foreground = FamoUI.Br("Famo.Ink2"),
            Margin = new Thickness(0, 0, 0, 12),
            TextWrapping = TextWrapping.Wrap,
        };
        sp.Children.Add(_status);

        _prompt = new TextBox
        {
            PlaceholderText = _selectedText is null ? "问 AI 一个问题" : "对选中文本下指令，或提问",
            AcceptsReturn = true,
            TextWrapping = TextWrapping.Wrap,
            MinHeight = 96,
            IsEnabled = askEnabled,
        };
        _result = new TextBox
        {
            Header = "回答",
            AcceptsReturn = true,
            TextWrapping = TextWrapping.Wrap,
            IsReadOnly = true,
            MinHeight = 180,
        };
        _send = new Button
        {
            Content = "发送",
            IsEnabled = askEnabled,
        };
        _send.Click += async (_, _) => await SendAsync();

        if (_selectedText is not null)
        {
            sp.Children.Add(FamoUI.Card("选中文本", FamoUI.RowFull(new TextBox
            {
                Text = _selectedText,
                AcceptsReturn = true,
                TextWrapping = TextWrapping.Wrap,
                IsReadOnly = true,
                MaxHeight = 160,
            })));
            sp.Children.Add(BuildToolboxSkills());
        }
        if (askEnabled)
        {
            sp.Children.Add(FamoUI.Card("任意提问", FamoUI.RowFull(_prompt), FamoUI.RowFull(BuildActions(), divider: true)));
        }
        if (askEnabled || _selectedText is not null)
            sp.Children.Add(FamoUI.Card("回答", FamoUI.RowFull(_result)));

        root.Content = sp;
        Content = root;
    }

    private FrameworkElement BuildToolboxSkills()
    {
        var grid = new Grid { ColumnSpacing = 8, RowSpacing = 8 };
        for (int column = 0; column < 3; column++)
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        int index = 0;
        foreach (AiSelectionSkillDefinition skill in AiSelectionSkills.BuiltIn)
        {
            if (!AiSelectionSkills.IsEnabled(App.Settings, skill.Id)) continue;
            int row = index / 3;
            if (row == grid.RowDefinitions.Count)
                grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            var button = new Button
            {
                Content = skill.Title,
                HorizontalAlignment = HorizontalAlignment.Stretch,
                IsEnabled = App.Settings.Ai.CloudEnabled,
            };
            button.Click += async (_, _) =>
            {
                if (skill.Id is "translation" or "research-assist")
                {
                    await RunToolboxSkillAsync(skill, button);
                    return;
                }
                App.ShowAiSelectionSkill(skill, _selectedText!, _replacement);
                Close();
            };
            Grid.SetRow(button, row);
            Grid.SetColumn(button, index % 3);
            grid.Children.Add(button);
            index++;
        }
        if (index == 0)
        {
            grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            grid.Children.Add(new TextBlock
            {
                Text = "技能平台中没有已启用的划词技能。",
                Foreground = FamoUI.Br("Famo.Ink2"),
            });
        }
        return FamoUI.Card("技能", FamoUI.RowFull(grid));
    }

    private async Task RunToolboxSkillAsync(AiSelectionSkillDefinition skill, Button button)
    {
        button.IsEnabled = false;
        SetStatus(skill.RunningMessage);
        try
        {
            var service = new AiSelectionSkillService(App.Settings, _providerStore, _secretStore);
            AiSelectionSkillResult response = await service.RunAsync(skill, _selectedText!, CancellationToken.None);
            string result = string.Join("\n\n", response.Candidates);
            _result.Text = result;
            _turns.Add(new AiChatTurn($"执行技能：{skill.Title}", result));
            SetStatus($"{skill.Title}已由 {response.Model} 返回，可继续追问或选择其他技能。");
        }
        catch (Exception ex)
        {
            SetStatus(ex.Message);
        }
        finally
        {
            button.IsEnabled = App.Settings.Ai.CloudEnabled;
        }
    }

    private FrameworkElement BuildActions()
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Margin = new Thickness(0, 12, 0, 0) };
        var copy = new Button { Content = "复制结果" };
        copy.Click += (_, _) => CopyResult();
        var copyAndClose = new Button { Content = "复制并关闭" };
        copyAndClose.Click += (_, _) =>
        {
            if (CopyResult()) Close();
        };
        var clear = new Button { Content = "清空" };
        clear.Click += (_, _) => Clear();
        row.Children.Add(_send);
        row.Children.Add(copy);
        row.Children.Add(copyAndClose);
        row.Children.Add(clear);
        return row;
    }

    private async Task SendAsync()
    {
        if (_selectedText is not null && !App.Settings.Ai.AskAnythingSkillEnabled)
        {
            SetStatus("任意提问已在技能平台关闭。");
            return;
        }
        string question = _prompt.Text;
        _send.IsEnabled = false;
        SetStatus("正在发送...");
        try
        {
            var client = new AiChatClient(App.Settings, _providerStore, _secretStore);
            AiChatResult response = await client.SendAsync(question, _turns, _selectedText, CancellationToken.None);
            _turns.Add(new AiChatTurn(question.Trim(), response.Text));
            _result.Text = response.Text;
            _prompt.Text = "";
            SetStatus($"已由 {response.Model} 返回（第 {_turns.Count} 轮，可继续追问）。");
        }
        catch (Exception ex)
        {
            SetStatus(ex.Message);
        }
        finally
        {
            _send.IsEnabled = true;
        }
    }

    private bool CopyResult()
    {
        if (string.IsNullOrWhiteSpace(_result.Text))
        {
            SetStatus("没有可复制的结果。");
            return false;
        }

        var data = new DataPackage();
        data.SetText(_result.Text);
        Clipboard.SetContent(data);
        SetStatus("结果已复制。");
        return true;
    }

    private void Clear()
    {
        _prompt.Text = "";
        _result.Text = "";
        _turns.Clear();
        SetStatus("已清空，开启新对话。");
    }

    private void SetStatus(string text)
    {
        if (_status != null) _status.Text = text;
    }
}
