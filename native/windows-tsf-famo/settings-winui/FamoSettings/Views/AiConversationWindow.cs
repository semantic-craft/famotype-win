using Famo.Settings.Core.Ai;
using Famo.Settings.Theming;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.ApplicationModel.DataTransfer;

namespace Famo.Settings.Views;

/// <summary>AI 对话窗口：显式发送、复制结果，不改写 Rime 会话或当前输入内容。</summary>
public sealed class AiConversationWindow : Window
{
    private readonly AiProviderProfileStore _providerStore = new();
    private readonly ISecretStore _secretStore = new WindowsCredentialSecretStore();
    private readonly string? _selectedText;
    /// <summary>已完成的问答轮，按序喂回后续请求（真·多轮）；client 侧自会截到历史上限。</summary>
    private readonly List<AiChatTurn> _turns = new();

    private TextBox _prompt = null!;
    private TextBox _result = null!;
    private TextBlock _status = null!;
    private Button _send = null!;

    public AiConversationWindow(string? selectedText = null)
    {
        _selectedText = selectedText;
        Title = "AI 对话";
        BuildContent();
        Activated += (_, _) => _prompt.DispatcherQueue.TryEnqueue(
            () => _prompt.Focus(FocusState.Programmatic));
    }

    private void BuildContent()
    {
        var root = new ScrollViewer
        {
            Padding = new Thickness(24),
            Background = FamoUI.Br("Famo.Bg"),
        };
        var sp = new StackPanel();
        sp.Children.Add(FamoUI.PaneHeader("AI 对话", _selectedText is null
            ? "只处理你在这里主动发送的文本；不会读取普通输入候选。"
            : "对选中文本下指令或提问；不会读取选区之外的普通输入候选。"));

        _status = new TextBlock
        {
            Text = _selectedText is null
                ? "输入问题后发送；结果只显示在本窗口，可手动复制。"
                : "对选中文本下指令：改写、排版、翻译、拟回复……也可以直接提问。",
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
        };
        _result = new TextBox
        {
            Header = "回答",
            AcceptsReturn = true,
            TextWrapping = TextWrapping.Wrap,
            IsReadOnly = true,
            MinHeight = 180,
        };
        _send = new Button { Content = "发送" };
        _send.Click += async (_, _) => await SendAsync();

        sp.Children.Add(FamoUI.Card("问题", FamoUI.RowFull(_prompt), FamoUI.RowFull(BuildActions(), divider: true)));
        sp.Children.Add(FamoUI.Card("结果", FamoUI.RowFull(_result)));

        root.Content = sp;
        Content = root;
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
