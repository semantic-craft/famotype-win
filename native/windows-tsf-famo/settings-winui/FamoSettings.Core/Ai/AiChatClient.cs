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
    public async Task<AiChatResult> SendAsync(
        string prompt,
        IReadOnlyList<AiChatTurn> history,
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
