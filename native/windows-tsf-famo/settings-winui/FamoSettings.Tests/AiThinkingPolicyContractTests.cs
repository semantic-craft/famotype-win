using System.Net;
using System.Text;
using Famo.Settings.Core;
using Famo.Settings.Core.Ai;
using Xunit;

namespace Famo.Settings.Tests;

/// <summary>
/// 「全技能一律关思考」的线上契约。命名带 ContractTests 是为了让 CI 的
/// `--filter FullyQualifiedName~ContractTests` 真的跑到它——否则这条策略在 CI 上无人看守。
///
/// 依据：2026-07-20 打真实百炼的 A/B（macOS 主线仓 scripts/Test-FamoAISkills.ps1 ± -ThinkingOff）。
/// 提示词优化 24.4s → 2.5s、任意提问第二轮 9.4s → 1.7s，四要素 / 专名保留 / 多轮承接 /
/// 多版本区分度断言逐条不变。改动前 Windows 侧一处思考开关都没有。
/// </summary>
public sealed class AiThinkingPolicyContractTests : IDisposable
{
    private readonly string _dir;
    private readonly string _file;
    private readonly FakeSecretStore _secrets = new();

    public AiThinkingPolicyContractTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "famo-ai-think-" + Guid.NewGuid().ToString("N"));
        _file = Path.Combine(_dir, "ai-providers.json");
    }

    public void Dispose()
    {
        if (Directory.Exists(_dir)) Directory.Delete(_dir, recursive: true);
    }

    [Theory]
    // DeepSeek Chat Completions 走 thinking:{"type":"disabled"}。
    [InlineData("deepseek-chat", "\"thinking\":{\"type\":\"disabled\"}", "enable_thinking")]
    // DeepSeek V4 Flash Responses API 走 reasoning.effort:none。
    [InlineData("deepseek-v4-flash", "\"reasoning\":{\"effort\":\"none\"}", "\"thinking\"")]
    // 千问 Responses API 走 reasoning.effort:none，不再使用即将弃用的 enable_thinking。
    [InlineData("qwen3.6-flash", "\"reasoning\":{\"effort\":\"none\"}", "enable_thinking")]
    public async Task SendAsync_DisablesThinkingForModelsThatSupportIt(
        string model, string expectedFragment, string forbiddenFragment)
    {
        string body = await CaptureRequestBodyAsync(model);

        Assert.Contains(expectedFragment, body);
        Assert.DoesNotContain(forbiddenFragment, body);
    }

    [Fact]
    public async Task SendAsync_OmitsThinkingFieldsForProvidersThatDoNotKnowThem()
    {
        // 别家（OpenAI / 智谱 / Gemini…）一个思考字段都不能带：请求体逐字节不变。
        // DashScope 会拒未知字段，无差别下发等于把技能在部分供应商上打死。
        string body = await CaptureRequestBodyAsync("gpt-4o");

        Assert.DoesNotContain("thinking", body);
        Assert.DoesNotContain("enable_thinking", body);
    }

    [Fact]
    public async Task SendAsync_KeepsTheExistingWireShapeAlongsideTheThinkingSwitch()
    {
        // 千问迁移到 Responses API 后，必须使用官方 input/store 线缆。
        string body = await CaptureRequestBodyAsync("qwen3.6-flash");

        Assert.Contains("\"model\":\"qwen3.6-flash\"", body);
        Assert.Contains("\"stream\":false", body);
        Assert.Contains("\"store\":false", body);
        Assert.Contains("\"input\":[", body);
        Assert.Contains("\"role\":\"user\"", body);
        Assert.DoesNotContain("\"messages\":", body);
    }

    private async Task<string> CaptureRequestBodyAsync(string model)
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Ai.CloudEnabled = true;
        AddDefaultProfile(model, "sk-secret");

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
        await client.SendAsync("写一段说明", CancellationToken.None);
        return captured;
    }

    private void AddDefaultProfile(string model, string key)
    {
        var service = new AiProviderProfileService(new AiProviderProfileStore(_file), _secrets);
        service.AddProfile(new AiProviderProfileDraft
        {
            DisplayName = "Probe",
            Endpoint = model.StartsWith("qwen", StringComparison.OrdinalIgnoreCase)
                ? QwenResponsesApi.BuildBeijingEndpoint("llm-thinking-test")
                : model.Equals(DeepSeekResponsesApi.FlashModel, StringComparison.OrdinalIgnoreCase)
                    ? DeepSeekResponsesApi.Endpoint
                    : "https://api.deepseek.com/v1/chat/completions",
            Model = model,
            ApiKey = key,
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
