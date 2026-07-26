using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;
using Famo.Settings.Core;
using Famo.Settings.Core.Ai;
using Famo.Settings.Core.Insertion;
using Famo.Settings.Core.QuickPhrases;
using Famo.Settings.Core.Selection;
using Famo.Settings.Interop;
using Famo.Settings.Theming;
using Famo.Settings.Views;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Documents;

namespace Famo.Settings;

/// <summary>
/// 法墨设置面板应用入口（unpackaged WinUI 3）。
/// </summary>
public partial class App : Application
{
    /// <summary>主窗口（供主题系统跟随系统明暗用）。</summary>
    public static Window? Window { get; private set; }

    /// <summary>表情/符号浮窗（学搜狗：状态栏触发；隐藏复用，常驻进程供秒拉起）。</summary>
    private static EmojiWindow? _emojiWindow;

    /// <summary>剪贴板历史浮窗（状态栏触发；不抢焦点，点选后回插原焦点）。</summary>
    private static ClipboardWindow? _clipboardWindow;

    /// <summary>AI 对话窗口（显式触发；不进入输入热路径）。</summary>
    private static AiConversationWindow? _aiConversationWindow;

    /// <summary>提示词快速选择器（显式触发；变量填充后粘贴到先前焦点窗口）。</summary>
    private static PromptPickerWindow? _promptPickerWindow;

    /// <summary>快捷短语选择器（显式触发；五笔不占用字母候选，直接粘贴到先前焦点窗口）。</summary>
    private static QuickPhrasePickerWindow? _quickPhrasePickerWindow;

    /// <summary>划词技能窗口（显式触发；先捕获选中文本，再显示复制型结果）。</summary>
    private static AiSelectionPolishWindow? _aiSelectionSkillWindow;

    /// <summary>UI 线程派发器（单实例重定向回调在后台线程，须切回 UI 线程操作窗口）。</summary>
    private static DispatcherQueue? _uiQueue;

    private static string? _pendingPromptContent;
    private static string? _pendingPromptStatus;
    private static readonly object ReloadStatusGate = new();
    private static readonly ConditionalWeakTable<TextBlock, ReloadStatusToken> ReloadStatusTokens = new();

    /// <summary>深链 page 是否为表情浮窗。</summary>
    private static bool IsEmojiPage(string? page) =>
        string.Equals(page, "emoji", StringComparison.OrdinalIgnoreCase);

    /// <summary>深链 page 是否为剪贴板历史浮窗。</summary>
    private static bool IsClipboardPanelPage(string? page) =>
        string.Equals(page, "clipboard-panel", StringComparison.OrdinalIgnoreCase);

    /// <summary>深链 page 是否为 AI 对话窗口。</summary>
    private static bool IsAiChatPage(string? page) =>
        string.Equals(page, "ai-chat", StringComparison.OrdinalIgnoreCase);

    /// <summary>深链 page 是否为提示词快速选择器。</summary>
    private static bool IsPromptPickerPage(string? page) =>
        string.Equals(page, "prompt-picker", StringComparison.OrdinalIgnoreCase);

    /// <summary>深链 page 是否为快捷短语选择器。</summary>
    private static bool IsQuickPhrasePickerPage(string? page) =>
        string.Equals(page, "quick-phrase-picker", StringComparison.OrdinalIgnoreCase);

    /// <summary>深链 page 是否为保存选中文本为提示词。</summary>
    private static bool IsPromptSaveSelectionPage(string? page) =>
        string.Equals(page, "prompt-save-selection", StringComparison.OrdinalIgnoreCase);

    /// <summary>深链 page 是否为 AI 润色选中窗口（保留给现有测试和入口）。</summary>
    private static bool IsAiPolishPage(string? page) =>
        string.Equals(page, "ai-polish", StringComparison.OrdinalIgnoreCase);

    /// <summary>按深链 page 解析内置划词技能。</summary>
    private static bool TryResolveAiSelectionSkillPage(string? page, out AiSelectionSkillDefinition? skill)
    {
        skill = AiSelectionSkills.FromPageId(page);
        return skill is not null;
    }

    /// <summary>显示表情浮窗（懒建 + 复用），在光标附近弹出且不抢焦点。</summary>
    public static void ShowEmoji()
    {
        ApplyTheme(); // 确保画刷资源就绪（emoji 单独启动时可能尚未铺色）
        _emojiWindow ??= new EmojiWindow();
        _emojiWindow.ShowNearCursor();
    }

    /// <summary>显示剪贴板历史浮窗（懒建 + 复用），在光标附近弹出且不抢焦点。</summary>
    public static void ShowClipboard()
    {
        ApplyTheme();
        _clipboardWindow ??= new ClipboardWindow();
        _clipboardWindow.ShowNearCursor();
    }

    /// <summary>显示 AI 对话窗口（懒建 + 复用），仅响应用户显式触发。</summary>
    public static void ShowAiConversation()
    {
        ShowAiConversation(selectedText: null, replacement: null);
    }

    private static void ShowAiConversation(string? selectedText, ITextInsertionService? replacement)
    {
        ApplyTheme();
        if (_aiConversationWindow is null || selectedText is not null)
        {
            _aiConversationWindow?.Close();
            var window = new AiConversationWindow(selectedText, replacement);
            _aiConversationWindow = window;
            window.Closed += (_, _) =>
            {
                if (ReferenceEquals(_aiConversationWindow, window)) _aiConversationWindow = null;
            };
        }
        _aiConversationWindow.Activate();
    }

    /// <summary>在窗口抢焦点前捕获明确选区；无选区时仍打开普通任意提问。</summary>
    public static void ShowAiConversationForSelection()
    {
        _ = ShowAiConversationForSelectionAsync();
    }

    private static async Task ShowAiConversationForSelectionAsync()
    {
        nint targetWindow = SendInputPasteCommandSender.CaptureForegroundWindow();
        (SelectedTextCaptureResult result, WindowsUiAutomationSelectionAnchor? anchor) =
            await CaptureSelectionForReplacementAsync();
        ITextInsertionService? replacement = anchor is not null
            ? TextInsertionServices.VerifiedClipboardPasteForTarget(targetWindow, anchor)
            : null;
        ShowAiConversation(
            result.Status == SelectedTextCaptureStatus.Success ? result.Text : null,
            replacement);
    }

    /// <summary>显示提示词选择器。创建窗口前捕获当前前台窗口作为粘贴目标。</summary>
    public static void ShowPromptPicker()
    {
        ApplyTheme();
        _promptPickerWindow?.Close();
        _promptPickerWindow = new PromptPickerWindow(TextInsertionServices.ClipboardPasteForForegroundTarget());
        _promptPickerWindow.Closed += (s, _) => { ((PromptPickerWindow)s).UnsubscribeTheme(); _promptPickerWindow = null; };
        _promptPickerWindow.ShowNearCursor();
    }

    /// <summary>显示快捷短语选择器。创建窗口前捕获当前前台窗口作为粘贴目标。</summary>
    public static void ShowQuickPhrasePicker()
    {
        ApplyTheme();
        _quickPhrasePickerWindow?.Close();
        _quickPhrasePickerWindow = new QuickPhrasePickerWindow(TextInsertionServices.ClipboardPasteForForegroundTarget());
        _quickPhrasePickerWindow.Closed += (s, _) => { ((QuickPhrasePickerWindow)s).UnsubscribeTheme(); _quickPhrasePickerWindow = null; };
        _quickPhrasePickerWindow.ShowNearCursor();
    }

    public static void OpenSettingsPage(string page) => OpenMainWindow(page);

    /// <summary>捕获当前选中文本并打开提示词库编辑器；捕获发生在主设置窗激活前。</summary>
    public static void ShowPromptSaveSelection()
    {
        _ = ShowPromptSaveSelectionAsync();
    }

    public static string? TakePendingPromptContent()
    {
        string? value = _pendingPromptContent;
        _pendingPromptContent = null;
        return value;
    }

    public static string? TakePendingPromptStatus()
    {
        string? value = _pendingPromptStatus;
        _pendingPromptStatus = null;
        return value;
    }

    private static async Task ShowPromptSaveSelectionAsync()
    {
        ApplyTheme();
        SelectedTextCaptureResult result = await BuildSelectionCaptureService().CaptureAsync(CancellationToken.None);
        if (result.Status == SelectedTextCaptureStatus.Success && !string.IsNullOrWhiteSpace(result.Text))
        {
            _pendingPromptContent = result.Text;
            _pendingPromptStatus = result.Source == SelectedTextCaptureSource.FocusedControl
                ? "已从当前控件读取选中文本，请补充标题、分类和标签后保存。"
                : "已通过剪贴板兜底读取选中文本，并已尽量恢复原剪贴板。";
        }
        else
        {
            _pendingPromptStatus = result.Status switch
            {
                SelectedTextCaptureStatus.SecureField => "当前焦点位于密码或安全输入框，已取消保存。",
                SelectedTextCaptureStatus.NoSelection => "未选中文本。请先在目标应用中选中文本，再保存为提示词。",
                _ => string.IsNullOrWhiteSpace(result.Error) ? "未能读取选中文本。" : result.Error,
            };
        }

        OpenMainWindow("prompt-library");
    }

    /// <summary>显示 AI 润色选中窗口。捕获必须尽量发生在窗口激活前，避免焦点落到本窗口。</summary>
    public static void ShowAiSelectionPolish()
    {
        ShowAiSelectionSkill(AiSelectionSkills.Polish);
    }

    public static void ShowAiSelectionSkill(AiSelectionSkillDefinition skill)
    {
        _ = ShowAiSelectionSkillAsync(skill);
    }

    public static void ShowAiSelectionSkill(
        AiSelectionSkillDefinition skill,
        string selectedText,
        ITextInsertionService? replacement)
    {
        _ = ShowCapturedAiSelectionSkillAsync(skill, selectedText, replacement);
    }

    private static async Task ShowAiSelectionSkillAsync(AiSelectionSkillDefinition skill)
    {
        ApplyTheme();
        if (!AiSelectionSkills.IsEnabled(Settings, skill.Id))
        {
            AiSelectionPolishWindow disabledWindow = CreateAiSelectionSkillWindow(skill, null);
            await disabledWindow.CaptureSelectionBeforeActivationAsync();
            disabledWindow.Activate();
            return;
        }
        nint targetWindow = SendInputPasteCommandSender.CaptureForegroundWindow();
        (SelectedTextCaptureResult result, WindowsUiAutomationSelectionAnchor? anchor) =
            await CaptureSelectionForReplacementAsync();
        ITextInsertionService? replacement = anchor is not null
            ? TextInsertionServices.VerifiedClipboardPasteForTarget(targetWindow, anchor)
            : null;
        AiSelectionPolishWindow window = CreateAiSelectionSkillWindow(skill, replacement);
        window.LoadCapturedSelection(result);
        window.Activate();
        await window.RunSkillAsync();
    }

    private static async Task ShowCapturedAiSelectionSkillAsync(
        AiSelectionSkillDefinition skill,
        string selectedText,
        ITextInsertionService? replacement)
    {
        ApplyTheme();
        AiSelectionPolishWindow window = CreateAiSelectionSkillWindow(skill, replacement);
        window.LoadCapturedSelection(selectedText);
        window.Activate();
        await window.RunSkillAsync();
    }

    private static AiSelectionPolishWindow CreateAiSelectionSkillWindow(
        AiSelectionSkillDefinition skill,
        ITextInsertionService? replacement)
    {
        _aiSelectionSkillWindow?.Close();
        var window = new AiSelectionPolishWindow(skill, replacement);
        _aiSelectionSkillWindow = window;
        window.Closed += (_, _) =>
        {
            if (ReferenceEquals(_aiSelectionSkillWindow, window))
                _aiSelectionSkillWindow = null;
        };
        return window;
    }

    /// <summary>当前进程加载的设置 store（单一真相源，供各页读写）。</summary>
    public static SettingsStore Store { get; } = new();

    /// <summary>当前设置快照。</summary>
    public static FamoSettings Settings { get; private set; } = new();

    /// <summary>按当前 store 的皮肤/明暗重绘整窗调色板（换肤即时生效）。</summary>
    public static void ApplyTheme() =>
        FamoTheme.Apply(Settings.Appearance.Skin, Settings.Appearance.AppearanceMode);

    /// <summary>
    /// Store.Load() 读取失败时会先备份损坏文件为 .bak 再抛出（SafeJsonFile.Read）；
    /// 这里接住异常，落日志后回退默认设置继续启动，不让设置文件损坏直接崩掉整个面板。
    /// </summary>
    private static FamoSettings LoadSettingsOrFallback()
    {
        try
        {
            return Store.Load();
        }
        catch (Exception ex)
        {
            FamoLog.Append($"设置文件损坏，回退默认设置：{Store.FilePath}：{ex.Message}");
            FamoSettings fallback = SettingsStore.CreateDefault();
            Store.Save(fallback);
            return fallback;
        }
    }

    public App()
    {
        this.InitializeComponent();
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        string[] argv = Environment.GetCommandLineArgs();

        // 无头 seed 模式：首启按当前用户复制 {app}\data -> %LOCALAPPDATA%\Famo，
        // 再落 settings store / 即时覆盖层后退出，用于安装器 postinstall。
        if (HasFlag(argv, "--seed-only"))
        {
            FirstLaunchSeeder.SeedFromInstalledData(force: HasFlag(argv, "--force"));
            Settings = LoadSettingsOrFallback();
            TryMigrateQuickPhraseArtifacts(triggerDeploy: false);
            TryWriteOverlays();
            Exit();
            return;
        }

        // 首启 seed：文件不存在则从内置默认落盘到 %LOCALAPPDATA%\Famo\famo-settings.json。
        Settings = LoadSettingsOrFallback();
        TryMigrateQuickPhraseArtifacts(triggerDeploy: true);

        // 启动即把即时层（外观②/开关①覆盖层）与 store 同步落盘，使安装首启(--seed-only)后
        // server 也能在 AddSession 读到当前默认；纯写文件，不触发任何 reload/部署。
        TryWriteOverlays();

        // 无头即时桶 demo：用与 UI 相同的 store 路径写一组 appearance 改动后退出。
        // 取证用，也是 S4 reload smoke 的挂点。
        if (HasFlag(argv, "--demo-appearance"))
        {
            Settings.Appearance.Skin = "wuda";
            Settings.Appearance.FontPoint = 18;
            Settings.Appearance.Orientation = "vertical";
            Settings.Appearance.Layout.CornerRadius = 12;
            SaveAndApplyInstant(); // 写 store + 生成 weasel.custom.yaml（即时桶）
            Exit();
            return;
        }

        // 捕获 UI 线程派发器，供单实例重定向回调切回。
        _uiQueue = DispatcherQueue.GetForCurrentThread();

        // --page <id> 深链：从托盘/悬浮面板拉起时直达指定页；emoji 为特例 → 表情浮窗，不开设置主窗。
        string? startPage = GetArgValue(argv, "--page");

        if (IsEmojiPage(startPage))
        {
            ShowEmoji(); // 只显浮窗；隐藏后进程常驻，供后续秒拉起
            return;
        }
        if (IsClipboardPanelPage(startPage))
        {
            ShowClipboard(); // 只显剪贴板浮窗；隐藏后进程常驻，供后续秒拉起
            return;
        }
        if (IsAiChatPage(startPage))
        {
            ShowAiConversationForSelection(); // 先捕获明确选区，再显 AI 对话窗口
            return;
        }
        if (IsPromptPickerPage(startPage))
        {
            ShowPromptPicker(); // 只显提示词选择器；不打开设置主窗
            return;
        }
        if (IsQuickPhrasePickerPage(startPage))
        {
            ShowQuickPhrasePicker(); // 只显快捷短语选择器；不打开设置主窗
            return;
        }
        if (IsPromptSaveSelectionPage(startPage))
        {
            ShowPromptSaveSelection(); // 捕获选区后打开提示词库编辑器
            return;
        }
        if (TryResolveAiSelectionSkillPage(startPage, out AiSelectionSkillDefinition? startSkill))
        {
            ShowAiSelectionSkill(startSkill!); // 只显划词技能窗口；不打开设置主窗
            return;
        }

        OpenMainWindow(startPage);
    }

    private static bool HasFlag(string[] argv, string flag) =>
        argv.Any(a => string.Equals(a, flag, StringComparison.OrdinalIgnoreCase));

    /// <summary>取 <c>--flag value</c> 形式的参数值；缺失返回 null。</summary>
    private static string? GetArgValue(string[] argv, string flag)
    {
        for (int i = 0; i < argv.Length - 1; i++)
        {
            if (string.Equals(argv[i], flag, StringComparison.OrdinalIgnoreCase))
            {
                return argv[i + 1];
            }
        }
        return null;
    }

    // ─────────────────────────── 单实例 handoff（Program.cs 调用）───────────────────────────
    private static string PendingPageFile => Path.Combine(FamoPaths.FamoDir, ".famo-settings-pending-page");

    /// <summary>重定向实例把请求的 --page 写到 handoff 文件，供主实例读取（不依赖激活参数携带命令行）。</summary>
    public static void WritePendingPage(string? page)
    {
        try
        {
            Directory.CreateDirectory(FamoPaths.FamoDir);
            File.WriteAllText(PendingPageFile, page ?? string.Empty);
        }
        catch { /* handoff 失败不阻断重定向；主实例置前仍生效 */ }
    }

    /// <summary>主实例收到重定向激活：读 handoff → UI 线程分流（表情浮窗 / 置前或新建主窗 + 导航）。</summary>
    public static void OnRedirected()
    {
        string? page = null;
        try
        {
            if (File.Exists(PendingPageFile))
            {
                page = File.ReadAllText(PendingPageFile).Trim();
                File.Delete(PendingPageFile);
            }
        }
        catch { /* 读 handoff 失败仅置前不导航 */ }

        DispatcherQueue? queue = _uiQueue ?? Window?.DispatcherQueue;
        if (queue is null) return;
        queue.TryEnqueue(() =>
        {
            if (IsEmojiPage(page))
            {
                ShowEmoji(); // 表情浮窗：在光标附近弹出，不抢焦点
                return;
            }
            if (IsClipboardPanelPage(page))
            {
                ShowClipboard(); // 剪贴板浮窗：在光标附近弹出，不抢焦点
                return;
            }
            if (IsAiChatPage(page))
            {
                ShowAiConversationForSelection(); // 单实例深链同样先捕获选区
                return;
            }
            if (IsPromptPickerPage(page))
            {
                ShowPromptPicker(); // 提示词选择器：变量填充后粘贴到先前焦点窗口
                return;
            }
            if (IsQuickPhrasePickerPage(page))
            {
                ShowQuickPhrasePicker(); // 快捷短语选择器：五笔不占字母候选，直接插入
                return;
            }
            if (IsPromptSaveSelectionPage(page))
            {
                ShowPromptSaveSelection(); // 捕获选区后打开提示词库编辑器
                return;
            }
            if (TryResolveAiSelectionSkillPage(page, out AiSelectionSkillDefinition? redirectedSkill))
            {
                ShowAiSelectionSkill(redirectedSkill!); // 划词技能窗口：显式触发，不进入输入热路径
                return;
            }

            // 非 emoji：置前主窗并导航；若主窗尚未建（此前仅以 emoji 启动）则新建。
            OpenMainWindow(page);
        });
    }

    private static void OpenMainWindow(string? page)
    {
        if (Window is null)
        {
            Window = new MainWindow(page);
            ApplyTheme();
            Window.Activate();
            return;
        }

        BringToFront(Window);
        if (Window is MainWindow mw && !string.IsNullOrEmpty(page))
        {
            mw.NavigateTo(page);
        }
    }

    private static SelectedTextCaptureService BuildSelectionCaptureService() =>
        new(
            new WindowsFocusedTextSelectionReader(),
            new ClipboardCopySelectionReader(
                new WindowsClipboardTextChannel(),
                new Win32CopyShortcutSender()));

    private static async Task<(SelectedTextCaptureResult Result, WindowsUiAutomationSelectionAnchor? Anchor)>
        CaptureSelectionForReplacementAsync()
    {
        WindowsUiAutomationSelectionAnchor? anchor =
            await WindowsUiAutomationSelectionAnchor.CaptureAsync(CancellationToken.None);
        if (anchor is not null)
        {
            return (SelectedTextCaptureResult.Success(
                anchor.Text, SelectedTextCaptureSource.FocusedControl), anchor);
        }
        return (await BuildSelectionCaptureService().CaptureAsync(CancellationToken.None), null);
    }

    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    private const int SW_RESTORE = 9;

    private static void BringToFront(Window window)
    {
        try
        {
            IntPtr hwnd = WinRT.Interop.WindowNative.GetWindowHandle(window);
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        }
        catch { /* 置前失败不致命 */ }
    }

    public static void ReportReloadResult(
        ReloadResult result,
        TextBlock status,
        string pending = "已发送应用命令。",
        string running = "正在应用配置…",
        string succeeded = "配置已生效。",
        string failedPrefix = "应用失败")
    {
        if (!result.Started)
        {
            _ = RememberReloadStatus(status, result.Command);
            SetReloadFailure(status, result, pending, running, succeeded, failedPrefix);
            return;
        }

        SetStatusText(status, pending);
        string command = result.Command;
        ReloadStatusToken token = RememberReloadStatus(status, command);
        Action<DeployQueueSnapshot>? handler = null;
        handler = snapshot =>
        {
            if (snapshot.RequestId != result.RequestId ||
                !string.Equals(snapshot.Command, command, StringComparison.Ordinal))
            {
                return;
            }

            bool terminal = snapshot.Status is DeployQueueStatus.Succeeded or DeployQueueStatus.Failed;
            if (!IsCurrentReloadStatus(status, token))
            {
                if (terminal && handler is not null)
                {
                    DeployService.QueueChanged -= handler;
                }

                return;
            }

            if (snapshot.Status == DeployQueueStatus.Running)
            {
                SetStatusText(status, running);
                return;
            }

            if (snapshot.Status == DeployQueueStatus.Succeeded)
            {
                SetStatusText(status, succeeded);
            }
            else if (snapshot.Status == DeployQueueStatus.Failed)
            {
                SetReloadFailure(
                    status,
                    result with
                    {
                        Error = snapshot.Error ?? result.Error,
                        RetryAvailable = snapshot.RetryAvailable,
                        RequestId = snapshot.RequestId,
                    },
                    pending,
                    running,
                    succeeded,
                    failedPrefix);
            }
            else
            {
                return;
            }

            if (handler is not null)
            {
                DeployService.QueueChanged -= handler;
            }

            ClearReloadStatus(status, token);
        };

        DeployService.QueueChanged += handler;
        handler(DeployService.GetQueueSnapshot());
    }

    private sealed class ReloadStatusToken
    {
        public ReloadStatusToken(string command) => Command = command;
        public string Command { get; }
    }

    private static ReloadStatusToken RememberReloadStatus(TextBlock status, string command)
    {
        var token = new ReloadStatusToken(command);
        lock (ReloadStatusGate)
        {
            ReloadStatusTokens.Remove(status);
            ReloadStatusTokens.Add(status, token);
        }

        return token;
    }

    private static bool IsCurrentReloadStatus(TextBlock status, ReloadStatusToken token)
    {
        lock (ReloadStatusGate)
        {
            return ReloadStatusTokens.TryGetValue(status, out ReloadStatusToken? current) &&
                   ReferenceEquals(current, token);
        }
    }

    private static void ClearReloadStatus(TextBlock status, ReloadStatusToken token)
    {
        lock (ReloadStatusGate)
        {
            if (ReloadStatusTokens.TryGetValue(status, out ReloadStatusToken? current) &&
                ReferenceEquals(current, token))
            {
                ReloadStatusTokens.Remove(status);
            }
        }
    }

    private static void SetStatusText(TextBlock status, string text)
    {
        DispatcherQueue? queue = status.DispatcherQueue;
        if (queue is not null && !queue.HasThreadAccess)
        {
            queue.TryEnqueue(() =>
            {
                status.Inlines.Clear();
                status.Text = text;
            });
            return;
        }

        status.Inlines.Clear();
        status.Text = text;
    }

    private static void SetReloadFailure(
        TextBlock status,
        ReloadResult result,
        string pending,
        string running,
        string succeeded,
        string failedPrefix)
    {
        void Apply()
        {
            status.Text = string.Empty;
            status.Inlines.Clear();
            status.Inlines.Add(new Run
            {
                Text = $"{failedPrefix}：{result.Error ?? "未知错误"}",
            });
            if (!result.RetryAvailable || result.RequestId == 0)
            {
                return;
            }

            status.Inlines.Add(new Run { Text = "；" });
            var retry = new Hyperlink();
            retry.Inlines.Add(new Run { Text = "重试" });
            retry.Click += (_, _) =>
            {
                ReloadResult retried = DeployService.Retry(result.RequestId);
                ReportReloadResult(
                    retried, status, pending, running, succeeded, failedPrefix);
            };
            status.Inlines.Add(retry);
            status.Inlines.Add(new Run { Text = "。" });
        }

        DispatcherQueue? queue = status.DispatcherQueue;
        if (queue is not null && !queue.HasThreadAccess)
        {
            queue.TryEnqueue(Apply);
            return;
        }

        Apply();
    }

    /// <summary>即时桶页保存：写 store + 生成 famo-style.yaml 覆盖层，触发 runtime control reload-style。
    /// 同时回填 weasel.custom.yaml 作为「将来一次 deploy / 全新安装」的基线（不触发部署）。
    /// 返回 <see cref="ReloadResult"/> 供调用页做真实的加载/错误提示（而非无脑显示"已应用"）。</summary>
    public static ReloadResult SaveAndApplyInstant()
    {
        Store.Save(Settings);
        ApplyTheme(); // 皮肤/明暗即时重绘整窗
        try
        {
            ConfigWriter.WriteStyleOverlay(Settings, FamoPaths.FamoDir);   // 即时真相源
            ConfigWriter.WriteInstantBucket(Settings, FamoPaths.FamoDir);  // deploy 回填基线
            return DeployService.ReloadStyle(); // fire-and-forget，零部署，不阻塞 UI
        }
        catch (Exception ex)
        {
            // 写 YAML / 拉起 deployer 失败不应阻断 UI；store 已落盘，下次启动读覆盖层兜底。
            return new ReloadResult(false, null, "(write famo-style.yaml failed)", ex.Message)
            {
                RetryAvailable = false,
            };
        }
    }

    /// <summary>即时开关页保存：写 store + 生成 famo-options.yaml，再触发 runtime control reload-options
    /// （server set_option 到各会话；零部署即时生效，①）。供简繁/中英标点/emoji 等开关用。
    /// 返回 <see cref="ReloadResult"/> 供调用页做真实的加载/错误提示（而非无脑显示"已应用"）。</summary>
    public static ReloadResult SaveAndApplyOption()
    {
        Store.Save(Settings);
        try
        {
            ConfigWriter.WriteOptionsOverlay(Settings, FamoPaths.FamoDir);
            return DeployService.ReloadOptions(); // fire-and-forget，零部署，不阻塞 UI
        }
        catch (Exception ex)
        {
            // 写 YAML / 拉起 deployer 失败不应阻断 UI；store 已落盘，下次启动读覆盖层兜底。
            return new ReloadResult(false, null, "(write famo-options.yaml failed)", ex.Message)
            {
                RetryAvailable = false,
            };
        }
    }

    /// <summary>启动时把即时层覆盖层（famo-style.yaml / famo-options.yaml）与 store 同步落盘。
    /// 仅写文件，不触发 reload；失败静默（不阻断启动）。</summary>
    private static void TryWriteOverlays()
    {
        try
        {
            ConfigWriter.WriteStyleOverlay(Settings, FamoPaths.FamoDir);
            ConfigWriter.WriteOptionsOverlay(Settings, FamoPaths.FamoDir);
            ConfigWriter.WriteSelectSchema(Settings, FamoPaths.FamoDir); // 输入方式真相源，供 AddSession 回放
        }
        catch
        {
            // 首启写覆盖层失败不应阻断启动；后续保存会再写。
        }
    }

    /// <summary>升级时从 JSON 真相源重放快捷短语派生表与 Rime 补丁；内容一致则零写入。</summary>
    private static void TryMigrateQuickPhraseArtifacts(bool triggerDeploy)
    {
        if (!File.Exists(FamoPaths.QuickPhrasesFile)) return;

        try
        {
            bool tableChanged = new QuickPhraseStore().WriteTableDb();
            string icePath = Path.Combine(FamoPaths.FamoDir, "rime_ice.custom.yaml");
            string? currentIce = File.Exists(icePath) ? File.ReadAllText(icePath) : null;
            bool patchChanged = currentIce != ConfigWriter.BuildRimeIceCustom(Settings, currentIce);
            if (!tableChanged && !patchChanged) return;

            ConfigWriter.WriteDeployBucket(Settings, FamoPaths.FamoDir);
            if (triggerDeploy) DeployService.TriggerReload(ReloadKind.FullDeploy);
        }
        catch (Exception ex)
        {
            FamoLog.Append($"快捷短语派生物迁移失败：{ex.Message}");
        }
    }

    /// <summary>即时输入方式页保存：写 store + 生成 famo-select-schema.txt，再触发 runtime control select-schema
    /// （server 读文件 select_schema 到各会话；秒切零部署，不重建 prism，②）。供拼音/双拼/五笔快切用。
    /// 返回 <see cref="ReloadResult"/> 供调用页做真实的加载/错误提示（而非无脑显示"已应用"）。</summary>
    public static ReloadResult SaveAndApplySchema()
    {
        Store.Save(Settings);
        try
        {
            ConfigWriter.WriteSelectSchema(Settings, FamoPaths.FamoDir);
            return DeployService.SelectSchema(); // fire-and-forget，零部署，不阻塞 UI
        }
        catch (Exception ex)
        {
            // 写文件 / 拉起 deployer 失败不应阻断 UI；store 已落盘，AddSession 兜底回放上次所选。
            return new ReloadResult(false, null, "(write famo-select-schema.txt failed)", ex.Message)
            {
                RetryAvailable = false,
            };
        }
    }

    /// <summary>部署桶页保存：写 store + 生成 default/rime_ice.custom.yaml，再触发部署
    /// （FamoRuntime --control deploy；方案/词典变更时重建 prism，异步不阻塞 UI）。
    /// 返回 <see cref="ReloadResult"/> 供调用页做真实的加载/错误提示（而非无脑显示"已应用"）。</summary>
    public static ReloadResult SaveAndApplyDeploy()
    {
        Store.Save(Settings);
        try
        {
            ConfigWriter.WriteDeployBucket(Settings, FamoPaths.FamoDir);
            return DeployService.TriggerReload(ReloadKind.FullDeploy); // fire-and-forget，不阻塞 UI
        }
        catch (Exception ex)
        {
            // 写 YAML 失败不应阻断 UI；store 已落盘。
            return new ReloadResult(false, null, "(write default/rime_ice.custom.yaml failed)", ex.Message)
            {
                RetryAvailable = false,
            };
        }
    }
}
