using System.Net;
using System.Text;
using System.Text.Json;
using Famo.Settings.Core;
using Famo.Settings.Core.Ai;
using Xunit;

namespace Famo.Settings.Tests;

/// <summary>
/// 任意提问「真·多轮」的线上契约（对齐 macOS issue #137）：历史轮以 user/assistant 对
/// 喂回、首轮与旧单轮逐字节同形、历史封顶丢最早的轮。断言落在编码后的 HTTP 请求体上。
/// </summary>
public sealed class AiChatHistoryContractTests : IDisposable
{
    private readonly string _dir;
    private readonly string _file;
    private readonly FakeSecretStore _secrets = new();

    public AiChatHistoryContractTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "famo-ai-history-" + Guid.NewGuid().ToString("N"));
        _file = Path.Combine(_dir, "ai-providers.json");
    }

    public void Dispose()
    {
        if (Directory.Exists(_dir)) Directory.Delete(_dir, recursive: true);
    }

    [Fact]
    public async Task SecondTurn_CarriesFirstTurnAsUserAssistantPair()
    {
        var history = new[] { new AiChatTurn("什么是善意取得？", "善意取得是指…") };

        IReadOnlyList<(string Role, string Content)> messages =
            await CaptureMessagesAsync("构成要件呢？", history);

        Assert.Equal(
            new[] { "system", "user", "assistant", "user" },
            messages.Select(m => m.Role).ToArray());
        Assert.Equal("什么是善意取得？", messages[1].Content);
        Assert.Equal("善意取得是指…", messages[2].Content);
        Assert.Equal("构成要件呢？", messages[3].Content);
    }

    [Fact]
    public async Task FirstTurn_MatchesTheOldSingleTurnShape()
    {
        // 空历史必须与旧单轮请求同形：system + user，不多一条消息。
        IReadOnlyList<(string Role, string Content)> messages =
            await CaptureMessagesAsync("什么是善意取得？", Array.Empty<AiChatTurn>());

        Assert.Equal(new[] { "system", "user" }, messages.Select(m => m.Role).ToArray());
    }

    [Fact]
    public async Task History_IsCappedByDroppingTheOldestTurns()
    {
        // 13 轮历史 → 只带最近 10 轮：token 不随轮数无限膨胀（与 macOS maxHistoryTurns 同策）。
        AiChatTurn[] history = Enumerable.Range(1, AiChatClient.MaxHistoryTurns + 3)
            .Select(i => new AiChatTurn($"问题{i}", $"回答{i}"))
            .ToArray();

        IReadOnlyList<(string Role, string Content)> messages =
            await CaptureMessagesAsync("新问题", history);

        // system + 10×(user,assistant) + user
        Assert.Equal(1 + AiChatClient.MaxHistoryTurns * 2 + 1, messages.Count);
        Assert.Equal("问题4", messages[1].Content);   // 最早的 3 轮被丢掉
        Assert.Equal("新问题", messages[^1].Content);
    }

    private async Task<IReadOnlyList<(string Role, string Content)>> CaptureMessagesAsync(
        string prompt, IReadOnlyList<AiChatTurn> history)
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Ai.CloudEnabled = true;
        AddDefaultProfile();

        string captured = "";
        var http = new HttpClient(new CaptureHandler(request =>
        {
            captured = request.Content!.ReadAsStringAsync().GetAwaiter().GetResult();
            return new HttpResponseMessage(HttpStatusCode.OK)
            {
                Content = new StringContent(
                    """{ "choices": [ { "message": { "content": "ok" } } ] }""",
                    Encoding.UTF8,
                    "application/json"),
            };
        }));

        var client = new AiChatClient(settings, new AiProviderProfileStore(_file), _secrets, http);
        await client.SendAsync(prompt, history, CancellationToken.None);

        using JsonDocument doc = JsonDocument.Parse(captured);
        return doc.RootElement.GetProperty("messages")
            .EnumerateArray()
            .Select(m => (m.GetProperty("role").GetString()!, m.GetProperty("content").GetString()!))
            .ToArray();
    }

    private void AddDefaultProfile()
    {
        var service = new AiProviderProfileService(new AiProviderProfileStore(_file), _secrets);
        service.AddProfile(new AiProviderProfileDraft
        {
            DisplayName = "Probe",
            Endpoint = "https://api.deepseek.com/v1/chat/completions",
            Model = "gpt-4o",
            ApiKey = "sk-secret",
            MakeDefault = true,
        });
    }

    private sealed class CaptureHandler(Func<HttpRequestMessage, HttpResponseMessage> send)
        : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(
            HttpRequestMessage request, CancellationToken cancellationToken)
            => Task.FromResult(send(request));
    }

    private sealed class FakeSecretStore : ISecretStore
    {
        private readonly Dictionary<string, string> _values = new(StringComparer.Ordinal);

        public void SetSecret(string name, string value) => _values[name] = value;

        public string? GetSecret(string name) => _values.TryGetValue(name, out string? v) ? v : null;

        public void DeleteSecret(string name) => _values.Remove(name);
    }
}
