using System.Net;
using System.Text;
using System.Text.Json;
using Famo.Settings.Core;
using Famo.Settings.Core.Ai;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class DeepSeekResponsesApiContractTests : IDisposable
{
    private readonly string _dir;
    private readonly string _file;
    private readonly FakeSecretStore _secrets = new();

    public DeepSeekResponsesApiContractTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "famo-deepseek-responses-" + Guid.NewGuid().ToString("N"));
        _file = Path.Combine(_dir, "ai-providers.json");
    }

    public void Dispose()
    {
        if (Directory.Exists(_dir)) Directory.Delete(_dir, recursive: true);
    }

    [Theory]
    [InlineData("https://api.deepseek.com/v1/chat/completions")]
    [InlineData("https://api.deepseek.com/chat/completions")]
    public void ResolveEndpoint_MigratesOfficialFlashChatRoutes(string configuredEndpoint)
    {
        Assert.Equal(
            DeepSeekResponsesApi.Endpoint,
            DeepSeekResponsesApi.ResolveEndpoint(
                new Uri(configuredEndpoint),
                DeepSeekResponsesApi.FlashModel).ToString());
    }

    [Fact]
    public void ResolveEndpoint_KeepsProOnChatCompletionsWhileResponsesDoesNotSupportIt()
    {
        var configured = new Uri("https://api.deepseek.com/chat/completions");

        Assert.Same(
            configured,
            DeepSeekResponsesApi.ResolveEndpoint(configured, "deepseek-v4-pro"));
    }

    [Theory]
    [InlineData("https://api.deepseek.com/v1/responses")]
    [InlineData("https://api.deepseek.com/Responses")]
    [InlineData("https://api.deepseek.com/responses?debug=true")]
    [InlineData("https://example.test/responses")]
    [InlineData("http://api.deepseek.com/responses")]
    public void IsResponsesEndpoint_RejectsAnythingOutsideTheExactOfficialRoute(string value)
    {
        Assert.False(DeepSeekResponsesApi.IsResponsesEndpoint(new Uri(value)));
    }

    [Fact]
    public async Task SendAsync_UsesResponsesWireShapeAndParsesOutputText()
    {
        FamoSettings settings = DeepSeekSettings();
        HttpRequestMessage? captured = null;
        string capturedBody = "";
        var http = new HttpClient(new CaptureHandler(request =>
        {
            captured = request;
            capturedBody = request.Content!.ReadAsStringAsync().GetAwaiter().GetResult();
            return JsonResponse(
                """
                {
                  "id": "resp_deepseek_test",
                  "object": "response",
                  "status": "completed",
                  "store": false,
                  "output": [
                    {
                      "id": "msg_deepseek_test",
                      "type": "message",
                      "role": "assistant",
                      "status": "completed",
                      "content": [
                        { "type": "output_text", "text": "DeepSeek 响应", "annotations": [] }
                      ]
                    }
                  ]
                }
                """);
        }));
        var client = new AiChatClient(
            settings, new AiProviderProfileStore(_file), _secrets, http);

        AiChatResult result = await client.SendAsync("你好", CancellationToken.None);

        Assert.Equal("DeepSeek 响应", result.Text);
        Assert.Equal(DeepSeekResponsesApi.FlashModel, result.Model);
        Assert.NotNull(captured);
        Assert.Equal(HttpMethod.Post, captured.Method);
        Assert.Equal(DeepSeekResponsesApi.Endpoint, captured.RequestUri!.ToString());
        Assert.Equal("Bearer", captured.Headers.Authorization!.Scheme);
        Assert.Equal("sk-deepseek-test", captured.Headers.Authorization.Parameter);

        using JsonDocument body = JsonDocument.Parse(capturedBody);
        JsonElement root = body.RootElement;
        Assert.Equal(DeepSeekResponsesApi.FlashModel, root.GetProperty("model").GetString());
        Assert.Equal(JsonValueKind.Array, root.GetProperty("input").ValueKind);
        Assert.False(root.GetProperty("stream").GetBoolean());
        Assert.Equal("none", root.GetProperty("reasoning").GetProperty("effort").GetString());
        Assert.False(root.TryGetProperty("messages", out _));
        Assert.False(root.TryGetProperty("thinking", out _));
        Assert.False(root.TryGetProperty("store", out _));
    }

    [Fact]
    public async Task SendAsync_WithDeepSeekSearch_UsesTheBuiltInToolAndProviderCredential()
    {
        FamoSettings settings = DeepSeekSettings();
        settings.Ai.AskWebSearchEnabled = true;
        settings.Ai.WebSearchBackend = WebSearchBackends.DeepSeek;
        string capturedBody = "";
        var http = new HttpClient(new CaptureHandler(request =>
        {
            capturedBody = request.Content!.ReadAsStringAsync().GetAwaiter().GetResult();
            return JsonResponse(
                """
                {
                  "status": "completed",
                  "output": [
                    {
                      "type": "web_search_call",
                      "status": "completed",
                      "action": {
                        "type": "search",
                        "query": "今日新闻",
                        "sources": [{ "type": "url", "url": "https://example.test/news" }]
                      }
                    },
                    {
                      "type": "message",
                      "role": "assistant",
                      "status": "completed",
                      "content": [
                        { "type": "output_text", "text": "联网结果", "annotations": [] }
                      ]
                    }
                  ]
                }
                """);
        }));
        var client = new AiChatClient(
            settings, new AiProviderProfileStore(_file), _secrets, http);

        AiChatResult result = await client.SendAsync("今天有什么新闻？", CancellationToken.None);

        Assert.Equal(
            "联网结果\n\n来源：\n- https://example.test/news",
            result.Text);
        Assert.Equal(
            WebSearchBackends.DisplayName(WebSearchBackends.DeepSeek),
            result.SearchProvider);
        using JsonDocument body = JsonDocument.Parse(capturedBody);
        JsonElement root = body.RootElement;
        JsonElement tool = Assert.Single(root.GetProperty("tools").EnumerateArray());
        Assert.Equal("web_search", tool.GetProperty("type").GetString());
        Assert.Equal("required", root.GetProperty("tool_choice").GetString());
        Assert.Null(_secrets.GetSecret(WebSearchBackends.SecretName(WebSearchBackends.DeepSeek)));
    }

    [Fact]
    public async Task SendAsync_WithJsonObjectResponse_UsesResponsesTextFormatAndJoinsOutputParts()
    {
        DeepSeekSettings();
        string capturedBody = "";
        var http = new HttpClient(new CaptureHandler(request =>
        {
            capturedBody = request.Content!.ReadAsStringAsync().GetAwaiter().GetResult();
            return JsonResponse(
                """
                {
                  "status": "completed",
                  "output": [
                    {
                      "type": "message",
                      "role": "assistant",
                      "status": "completed",
                      "content": [
                        { "type": "output_text", "text": "{\"candidates\":[", "annotations": [] },
                        { "type": "output_text", "text": "\"改写结果\"]}", "annotations": [] }
                      ]
                    }
                  ]
                }
                """);
        }));
        var client = new AiProviderChatCompletionClient(
            new AiProviderProfileStore(_file), _secrets, http);

        AiProviderChatCompletionResult result = await client.SendAsync(
            [new AiProviderChatMessage("user", "请返回 JSON。")],
            CancellationToken.None,
            jsonObjectResponse: true);

        Assert.Equal("{\"candidates\":[\"改写结果\"]}", result.Text);
        using JsonDocument body = JsonDocument.Parse(capturedBody);
        JsonElement root = body.RootElement;
        Assert.Equal(
            "json_object",
            root.GetProperty("text").GetProperty("format").GetProperty("type").GetString());
        Assert.Equal("developer", root.GetProperty("input")[0].GetProperty("role").GetString());
        Assert.False(root.TryGetProperty("response_format", out _));
    }

    private FamoSettings DeepSeekSettings()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Ai.CloudEnabled = true;
        var service = new AiProviderProfileService(new AiProviderProfileStore(_file), _secrets);
        service.AddProfile(new AiProviderProfileDraft
        {
            DisplayName = "DeepSeek",
            Endpoint = "https://api.deepseek.com/v1/chat/completions",
            Model = DeepSeekResponsesApi.FlashModel,
            ApiKey = "sk-deepseek-test",
            MakeDefault = true,
        });
        return settings;
    }

    private static HttpResponseMessage JsonResponse(string json) =>
        new(HttpStatusCode.OK)
        {
            Content = new StringContent(json, Encoding.UTF8, "application/json"),
        };

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

        public string? GetSecret(string name) => _values.TryGetValue(name, out string? value) ? value : null;

        public void DeleteSecret(string name) => _values.Remove(name);
    }
}
