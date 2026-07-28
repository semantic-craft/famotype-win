using System.Diagnostics;
using Famo.Settings.Core.Ai;
using Famo.Settings.Core.Insertion;
using Famo.Settings.Core.Legal;
using Famo.Settings.Core.Selection;
using Famo.Settings.Interop;
using Famo.Settings.Theming;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.ApplicationModel.DataTransfer;

namespace Famo.Settings.Views;

/// <summary>单个划词技能的独立窗口。</summary>
public sealed class AiSelectionPolishWindow : Window
{
    private readonly AiProviderProfileStore _providerStore = new();
    private readonly ISecretStore _secretStore = new WindowsCredentialSecretStore();
    private readonly SelectedTextCaptureService _captureService;
    private readonly AiSelectionSkillDefinition _skill;
    private readonly ITextInsertionService? _replacement;
    private readonly WindowsForegroundWindowTarget? _focusTarget;

    private TextBlock _status = null!;
    private TextBox _selection = null!;
    private StackPanel _candidateList = null!;
    private string? _capturedText;
    private int _generation;

    /// <summary>提示词优化的澄清问答累积，逐轮喂回；每次重新捕获草稿即清空。</summary>
    private readonly List<PromptClarification> _clarifications = [];

    public AiSelectionSkillDefinition Skill => _skill;

    public AiSelectionPolishWindow()
        : this(AiSelectionSkills.Polish, null, WindowsForegroundWindowTarget.CaptureForeground())
    {
    }

    public AiSelectionPolishWindow(AiSelectionSkillDefinition skill)
        : this(skill, null, WindowsForegroundWindowTarget.CaptureForeground())
    {
    }

    public AiSelectionPolishWindow(
        AiSelectionSkillDefinition skill,
        ITextInsertionService? replacement,
        WindowsForegroundWindowTarget? focusTarget)
    {
        _skill = skill;
        _replacement = replacement;
        _focusTarget = focusTarget;
        Title = skill.Title;
        _captureService = new SelectedTextCaptureService(
            new WindowsFocusedTextSelectionReader(),
            new ClipboardCopySelectionReader(
                new WindowsClipboardTextChannel(),
                new Win32CopyShortcutSender()));
        BuildContent();
        Closed += (_, _) => _focusTarget?.TryRestore();
    }

    public void LoadCapturedSelection(string selectedText)
    {
        ++_generation;
        _candidateList.Children.Clear();
        _clarifications.Clear();
        if (!AiSelectionSkills.IsEnabled(App.Settings, _skill.Id))
        {
            _capturedText = null;
            _selection.Text = "";
            SetStatus("该技能已在技能平台关闭。");
            return;
        }
        if (string.IsNullOrWhiteSpace(selectedText))
        {
            _capturedText = null;
            _selection.Text = "";
            SetStatus("未选中文本。");
            return;
        }
        _capturedText = selectedText;
        _selection.Text = selectedText;
        SetStatus("已从划词工具箱传入当前选区。");
    }

    public void LoadCapturedSelection(SelectedTextCaptureResult result)
    {
        if (result.Status == SelectedTextCaptureStatus.Success && result.Text is not null)
        {
            LoadCapturedSelection(result.Text);
            return;
        }
        ++_generation;
        _candidateList.Children.Clear();
        _clarifications.Clear();
        _capturedText = null;
        _selection.Text = "";
        SetStatus(UserMessage(result));
    }

    public async Task CaptureSelectionBeforeActivationAsync()
    {
        int my = ++_generation;
        _candidateList.Children.Clear();
        _capturedText = null;
        _clarifications.Clear();

        if (!AiSelectionSkills.IsEnabled(App.Settings, _skill.Id))
        {
            SetStatus("该技能已在设置中关闭，可到「技能平台」重新开启。");
            return;
        }

        SetStatus("正在读取当前选中文本...");
        SelectedTextCaptureResult result = await _captureService.CaptureAsync(CancellationToken.None);
        if (my != _generation) return;
        if (result.Status == SelectedTextCaptureStatus.Success && !string.IsNullOrWhiteSpace(result.Text))
        {
            _capturedText = result.Text;
            _selection.Text = result.Text;
            SetStatus(result.Source == SelectedTextCaptureSource.FocusedControl
                ? "已通过 Windows UI Automation 捕获选中文本。"
                : "已通过剪贴板兜底捕获选中文本，并已尝试恢复原剪贴板。");
            return;
        }

        _selection.Text = "";
        SetStatus(UserMessage(result));
    }

    public async Task RunPolishAsync()
    {
        await RunSkillAsync();
    }

    public async Task RunSkillAsync()
    {
        int my = _generation;
        if (string.IsNullOrWhiteSpace(_capturedText))
        {
            return;
        }
        string capturedText = _capturedText;

        // 提示词优化走两态契约（终稿 or 反问澄清），不是 {"candidates":[…]}，另走一条渲染。
        if (_skill.Id == AiSelectionSkills.PromptOptimize.Id)
        {
            await RunPromptOptimizeAsync(my, capturedText);
            return;
        }

        SetStatus(_skill.RunningMessage);
        _candidateList.Children.Clear();
        try
        {
            var service = new AiSelectionSkillService(App.Settings, _providerStore, _secretStore);
            AiSelectionSkillResult result = await service.RunAsync(_skill, capturedText, CancellationToken.None);
            if (my != _generation) return;
            RenderCandidates(result.Candidates);
            SetStatus($"已由 {result.Model} 返回 {result.Candidates.Count} 个候选。");
        }
        catch (Exception ex)
        {
            if (my != _generation) return;
            SetStatus(ex.Message);
        }
    }

    private async Task RunPromptOptimizeAsync(int my, string draft)
    {
        SetStatus(_skill.RunningMessage);
        _candidateList.Children.Clear();
        try
        {
            var service = new AiSelectionSkillService(App.Settings, _providerStore, _secretStore);
            PromptOptimizeOutcome outcome = await service.OptimizePromptAsync(
                draft, _clarifications, CancellationToken.None);
            if (my != _generation) return;

            if (outcome.NeedsClarification)
            {
                RenderClarification(outcome.Questions, draft);
                SetStatus("草稿缺了意图要素：回答下面的问题后再优化。");
                return;
            }

            RenderCandidates([outcome.FinalPrompt!]);
            SetStatus($"已由 {outcome.Model} 产出终稿；确认后复制替换草稿。");
        }
        catch (Exception ex)
        {
            if (my != _generation) return;
            SetStatus(ex.Message);
        }
    }

    /// <summary>反问澄清：逐题给一个补答框，补完原地重跑。问答累积进 <see cref="_clarifications"/>，
    /// 所以模型再问一轮时前面的补答不会丢。</summary>
    private void RenderClarification(IReadOnlyList<string> questions, string draft)
    {
        _candidateList.Children.Clear();
        var answers = new List<(string Question, TextBox Box)>();

        for (int i = 0; i < questions.Count; i++)
        {
            var box = new TextBox { TextWrapping = TextWrapping.Wrap };
            answers.Add((questions[i], box));

            var panel = new StackPanel { Spacing = 6 };
            panel.Children.Add(new TextBlock
            {
                Text = $"{i + 1}. {questions[i]}",
                TextWrapping = TextWrapping.Wrap,
            });
            panel.Children.Add(box);
            _candidateList.Children.Add(panel);
        }

        var rerun = new Button { Content = "补答后重新优化" };
        rerun.Click += async (_, _) =>
        {
            var answered = answers
                .Where(a => !string.IsNullOrWhiteSpace(a.Box.Text))
                .Select(a => new PromptClarification(a.Question, a.Box.Text.Trim()))
                .ToList();
            if (answered.Count == 0)
            {
                // 一个字都没补就重跑只会拿回同一批问题，白花一次调用。
                SetStatus("请先至少回答一个问题，再重新优化。");
                return;
            }

            _clarifications.AddRange(answered);
            await RunPromptOptimizeAsync(_generation, draft);
        };
        _candidateList.Children.Add(rerun);
    }

    private void BuildContent()
    {
        var root = new ScrollViewer
        {
            Padding = new Thickness(24),
            Background = FamoUI.Br("Famo.Bg"),
        };
        var sp = new StackPanel();
        sp.Children.Add(FamoUI.PaneHeader(_skill.Title, "只处理你这次明确选中的文本；可复制结果，改写型技能也可确认替换原选区。"));

        _status = new TextBlock
        {
            Text = "从菜单打开后会先读取当前选中文本。",
            FontSize = 12.5,
            Foreground = FamoUI.Br("Famo.Ink2"),
            Margin = new Thickness(0, 0, 0, 12),
            TextWrapping = TextWrapping.Wrap,
        };
        sp.Children.Add(_status);

        _selection = new TextBox
        {
            Header = "选中文本",
            AcceptsReturn = true,
            TextWrapping = TextWrapping.Wrap,
            IsReadOnly = true,
            MinHeight = 112,
        };

        _candidateList = new StackPanel { Spacing = 10 };
        sp.Children.Add(FamoUI.Card("输入", FamoUI.RowFull(_selection)));

        // 检索向技能（来源核验/辅助检索）附带跳库深链：本地拼 URL 开默认浏览器，
        // 不经过 AI——云端失败或没配 key 时用户照样能一键跳到权威库核对。
        if (_skill.Id is "source-check" or "research-assist")
        {
            sp.Children.Add(FamoUI.Card("跳库检索", FamoUI.RowFull(BuildJumpRow())));
        }

        sp.Children.Add(FamoUI.Card($"{_skill.Title}结果", FamoUI.RowFull(_candidateList)));

        root.Content = sp;
        Content = root;
    }

    private void RenderCandidates(IReadOnlyList<string> candidates)
    {
        _candidateList.Children.Clear();
        if (candidates.Count == 0)
        {
            _candidateList.Children.Add(new TextBlock
            {
                Text = "没有可复制的结果。",
                Foreground = FamoUI.Br("Famo.Ink2"),
                TextWrapping = TextWrapping.Wrap,
            });
            return;
        }

        for (int i = 0; i < candidates.Count; i++)
        {
            string candidate = candidates[i];
            var text = new TextBox
            {
                Header = $"{_skill.ResultTitle} {i + 1}",
                Text = candidate,
                AcceptsReturn = true,
                TextWrapping = TextWrapping.Wrap,
                IsReadOnly = true,
                MinHeight = 72,
            };
            var copy = new Button { Content = "复制" };
            copy.Click += (_, _) => CopyCandidate(candidate);

            var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            actions.Children.Add(copy);
            if (_replacement is not null && CanReplaceSelection())
            {
                var replace = new Button { Content = "确认替换原选区" };
                replace.Click += async (_, _) => await ReplaceCandidateAsync(candidate, replace);
                actions.Children.Add(replace);
            }

            var panel = new StackPanel { Spacing = 8 };
            panel.Children.Add(text);
            panel.Children.Add(actions);
            _candidateList.Children.Add(panel);
        }
    }

    private bool CanReplaceSelection() =>
        _skill.Id is "polish" or "publish-formatting" or "prompt-optimize";

    private async Task ReplaceCandidateAsync(string text, Button button)
    {
        if (_replacement is null) return;
        button.IsEnabled = false;
        TextInsertionResult result = await _replacement.InsertAsync(text, CancellationToken.None);
        SetStatus(result.Message);
        if (result.Success)
            Close();
        else
            button.IsEnabled = true;
    }

    private FrameworkElement BuildJumpRow()
    {
        var panel = new StackPanel { Spacing = 8 };
        panel.Children.Add(new TextBlock
        {
            Text = "用选中文本在权威库中直接检索（不经过 AI）：",
            FontSize = 12.5,
            Foreground = FamoUI.Br("Famo.Ink2"),
            TextWrapping = TextWrapping.Wrap,
        });

        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        foreach (LegalSearchSite site in LegalSearchRouter.Sites)
        {
            var button = new Button { Content = site.DisplayName };
            button.Click += (_, _) => OpenSite(site);
            buttons.Children.Add(button);
        }
        panel.Children.Add(buttons);
        return panel;
    }

    private void OpenSite(LegalSearchSite site)
    {
        if (string.IsNullOrWhiteSpace(_capturedText))
        {
            SetStatus("未选中文本，无法跳库检索。");
            return;
        }

        try
        {
            string url = LegalSearchRouter.SearchUrl(
                site, LegalSearchRouter.NormalizeSelection(_capturedText));
            Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
            SetStatus($"已在浏览器打开「{site.DisplayName}」检索。");
        }
        catch (Exception ex)
        {
            SetStatus(ex.Message);
        }
    }

    private void CopyCandidate(string text)
    {
        if (string.IsNullOrWhiteSpace(text))
        {
            SetStatus("没有可复制的结果。");
            return;
        }

        var data = new DataPackage();
        data.SetText(text);
        Clipboard.SetContent(data);
        SetStatus(_skill.CopiedMessage);
    }

    private string UserMessage(SelectedTextCaptureResult result) =>
        result.Status switch
        {
            SelectedTextCaptureStatus.NoSelection => $"未选中文本。请先在目标应用中选中文本，再打开 {_skill.Title}。",
            SelectedTextCaptureStatus.SecureField => "当前焦点位于密码或安全输入框，已取消读取选中文本。",
            SelectedTextCaptureStatus.Unavailable => string.IsNullOrWhiteSpace(result.Error)
                ? "当前控件不支持读取选中文本。"
                : result.Error,
            SelectedTextCaptureStatus.Failed => string.IsNullOrWhiteSpace(result.Error)
                ? "读取选中文本失败。"
                : result.Error,
            _ => "未选中文本。",
        };

    private void SetStatus(string text)
    {
        if (_status != null) _status.Text = text;
    }
}
