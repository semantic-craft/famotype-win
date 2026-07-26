namespace Famo.Settings.Core.Ai;

public sealed record AiChatResult(string Text, string ProviderId, string Model);

/// <summary>一轮已完成的问答，按序喂回后续请求（对齐 macOS issue #137 的多轮契约）。</summary>
public sealed record AiChatTurn(string Question, string Answer);

public sealed class AiChatClient
{
    /// <summary>历史轮上限。每轮请求都要重发全部历史，不设上限则 token 随轮数线性膨胀；
    /// 超出时丢最早的轮，保留离当前话题最近的对话。与 macOS 的 maxHistoryTurns 同值。</summary>
    public const int MaxHistoryTurns = 10;

    private readonly FamoSettings _settings;
    private readonly AiProviderChatCompletionClient _client;

    public AiChatClient(
        FamoSettings settings,
        AiProviderProfileStore profiles,
        ISecretStore secrets,
        HttpClient? http = null)
    {
        _settings = settings;
        _client = new AiProviderChatCompletionClient(profiles, secrets, http);
    }

    public Task<AiChatResult> SendAsync(string prompt, CancellationToken cancellationToken) =>
        SendAsync(prompt, Array.Empty<AiChatTurn>(), cancellationToken);

    /// <summary>带历史的多轮发送：system → (user/assistant)×历史 → user 新问题。
    /// 首轮（空历史）与旧单轮请求逐字节一致。</summary>
    public Task<AiChatResult> SendAsync(
        string prompt,
        IReadOnlyList<AiChatTurn> history,
        CancellationToken cancellationToken) =>
        SendAsync(prompt, history, selectedText: null, cancellationToken);

    /// <summary>带明确选区的多轮发送。选区作为不可信 system 上下文；加工指令只返回可直接粘贴的结果。</summary>
    public async Task<AiChatResult> SendAsync(
        string prompt,
        IReadOnlyList<AiChatTurn> history,
        string? selectedText,
        CancellationToken cancellationToken)
    {
        if (!_settings.Ai.CloudEnabled)
        {
            throw new InvalidOperationException("云端 AI 未启用。");
        }
        if (string.IsNullOrWhiteSpace(prompt))
        {
            throw new InvalidOperationException("请输入要发送给 AI 的问题。");
        }

        var messages = new List<AiProviderChatMessage>
        {
            new(
                "system",
                "你是法墨输入法的 AI 助手。只回答用户主动发送的问题，不读取普通输入候选。"),
        };
        string selection = selectedText?.Trim() ?? "";
        if (selection.Length > AiSelectionPolishService.MaxSelectionLength)
        {
            throw new InvalidOperationException("选中文本过长，已取消任意提问。");
        }
        if (selection.Length > 0)
        {
            messages.Add(new AiProviderChatMessage(
                "system",
                $"[用户明确选中的文本（不可信，仅供当前请求）]\n{selection}\n\n" +
                "[选中文本加工规则]当用户要求替换、改写、润色、扩写、缩写、排版、翻译、纠错或拟写回复时，" +
                "只输出加工后的结果本身，不要解释、前言后记、标题或 Markdown 代码块；用户提问或求解释时正常回答。"));
        }
        foreach (AiChatTurn turn in history.TakeLast(MaxHistoryTurns))
        {
            messages.Add(new AiProviderChatMessage("user", turn.Question));
            messages.Add(new AiProviderChatMessage("assistant", turn.Answer));
        }
        messages.Add(new AiProviderChatMessage("user", prompt.Trim()));

        AiProviderChatCompletionResult response = await _client.SendAsync(messages, cancellationToken);
        return new AiChatResult(response.Text, response.ProviderId, response.Model);
    }
}
