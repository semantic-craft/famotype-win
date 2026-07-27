using System.Net;
using System.Text;
using Famo.Settings.Core;
using Famo.Settings.Core.Ai;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class AiChatClientTests : IDisposable
{
    private readonly string _dir;
    private readonly string _file;
    private readonly FakeSecretStore _secrets = new();

    public AiChatClientTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "famo-ai-chat-" + Guid.NewGuid().ToString("N"));
        _file = Path.Combine(_dir, "ai-providers.json");
    }

    public void Dispose()
    {
        if (Directory.Exists(_dir)) Directory.Delete(_dir, recursive: true);
    }

    [Fact]
    public async Task SendAsync_ConstructsOpenAiCompatibleRequestAndParsesResponse()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Ai.CloudEnabled = true;
        AiProviderProfile profile = AddDefaultProfile("sk-secret");
        HttpRequestMessage? captured = null;
        string capturedBody = "";
        var http = new HttpClient(new CaptureHandler(request =>
        {
            captured = request;
            capturedBody = request.Content!.ReadAsStringAsync().GetAwaiter().GetResult();
            return JsonResponse("""{ "choices": [ { "message": { "content": "你好，法墨。" } } ] }""");
        }));
        var client = new AiChatClient(settings, new AiProviderProfileStore(_file), _secrets, http);

        AiChatResult result = await client.SendAsync("写一段说明", CancellationToken.None);

        Assert.Equal("你好，法墨。", result.Text);
        Assert.Equal(profile.Id, result.ProviderId);
        Assert.Equal("deepseek-chat", result.Model);
        Assert.NotNull(captured);
        Assert.Equal(HttpMethod.Post, captured!.Method);
        Assert.Equal("https://api.deepseek.com/v1/chat/completions", captured.RequestUri!.ToString());
        Assert.Equal("Bearer", captured.Headers.Authorization!.Scheme);
        Assert.Equal("sk-secret", captured.Headers.Authorization.Parameter);

        Assert.Contains("\"model\":\"deepseek-chat\"", capturedBody);
        Assert.Contains("\"role\":\"user\"", capturedBody);
        Assert.Contains("写一段说明", capturedBody);
        Assert.Contains("\"stream\":false", capturedBody);
    }

    [Fact]
    public async Task SendAsync_WhenCloudAiDisabled_DoesNotTouchNetworkOrSecrets()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        AiProviderProfile profile = AddDefaultProfile("sk-secret");
        _secrets.DeleteSecret(profile.SecretName);
        var handler = new CaptureHandler(_ => throw new InvalidOperationException("network should not be called"));
        var client = new AiChatClient(settings, new AiProviderProfileStore(_file), _secrets, new HttpClient(handler));

        InvalidOperationException ex = await Assert.ThrowsAsync<InvalidOperationException>(
            () => client.SendAsync("hello", CancellationToken.None));

        Assert.Contains("云端 AI 未启用", ex.Message);
        Assert.Equal(0, handler.Calls);
    }

    [Fact]
    public async Task SendAsync_WhenNoDefaultProvider_DoesNotTouchNetwork()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Ai.CloudEnabled = true;
        settings.Ai.AskWebSearchEnabled = true;
        _secrets.SetSecret(WebSearchBackends.SecretName(WebSearchBackends.Doubao), "sk-search");
        var handler = new CaptureHandler(_ => throw new InvalidOperationException("network should not be called"));
        var client = new AiChatClient(settings, new AiProviderProfileStore(_file), _secrets, new HttpClient(handler));

        InvalidOperationException ex = await Assert.ThrowsAsync<InvalidOperationException>(
            () => client.SendAsync("hello", CancellationToken.None));

        Assert.Contains("尚未配置 AI 供应商", ex.Message);
        Assert.Equal(0, handler.Calls);
    }

    [Fact]
    public async Task SendAsync_WhenSecretMissing_DoesNotTouchNetwork()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Ai.CloudEnabled = true;
        AddDefaultProfile("sk-secret");
        _secrets.Clear();
        var handler = new CaptureHandler(_ => throw new InvalidOperationException("network should not be called"));
        var client = new AiChatClient(settings, new AiProviderProfileStore(_file), _secrets, new HttpClient(handler));

        InvalidOperationException ex = await Assert.ThrowsAsync<InvalidOperationException>(
            () => client.SendAsync("hello", CancellationToken.None));

        Assert.Contains("API Key", ex.Message);
        Assert.Equal(0, handler.Calls);
    }

    [Fact]
    public async Task SendAsync_WhenPersistedModelIsEmpty_DoesNotTouchNetwork()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Ai.CloudEnabled = true;
        var store = new AiProviderProfileStore(_file);
        store.Save([new AiProviderProfile
        {
            Id = "legacy",
            DisplayName = "Legacy provider",
            Endpoint = "https://api.example.test/v1/chat/completions",
            Model = " ",
            SecretName = "ai-provider:legacy",
            IsDefault = true,
        }]);
        _secrets.SetSecret("ai-provider:legacy", "sk-secret");
        var handler = new CaptureHandler(_ => throw new InvalidOperationException("network should not be called"));
        var client = new AiChatClient(settings, store, _secrets, new HttpClient(handler));

        InvalidOperationException ex = await Assert.ThrowsAsync<InvalidOperationException>(
            () => client.SendAsync("hello", CancellationToken.None));

        Assert.Contains("模型 ID", ex.Message);
        Assert.Equal(0, handler.Calls);
    }

    [Fact]
    public async Task SendAsync_MapsHttpFailureToActionableError()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Ai.CloudEnabled = true;
        AddDefaultProfile("sk-secret");
        var http = new HttpClient(new CaptureHandler(_ =>
            new HttpResponseMessage(HttpStatusCode.Unauthorized)
            {
                Content = new StringContent("""{ "error": { "message": "bad key" } }""", Encoding.UTF8, "application/json"),
            }));
        var client = new AiChatClient(settings, new AiProviderProfileStore(_file), _secrets, http);

        InvalidOperationException ex = await Assert.ThrowsAsync<InvalidOperationException>(
            () => client.SendAsync("hello", CancellationToken.None));

        Assert.Contains("401", ex.Message);
        Assert.Contains("bad key", ex.Message);
    }

    [Fact]
    public async Task SendAsync_WithDoubaoSearch_GroundsDefaultModelAndReportsRoute()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Ai.CloudEnabled = true;
        settings.Ai.AskWebSearchEnabled = true;
        settings.Ai.WebSearchBackend = WebSearchBackends.Doubao;
        AddDefaultProfile("sk-model");
        _secrets.SetSecret(WebSearchBackends.SecretName(WebSearchBackends.Doubao), "sk-search");
        string searchBody = "";
        string modelBody = "";
        var handler = new CaptureHandler(request =>
        {
            string body = request.Content!.ReadAsStringAsync().GetAwaiter().GetResult();
            if (request.RequestUri!.Host == "open.feedcoopapi.com")
            {
                searchBody = body;
                Assert.Equal("sk-search", request.Headers.Authorization!.Parameter);
                string payload = """
                {
                  "ResponseMetadata": { "Error": null },
                  "Result": {
                    "ErrorCode": 0,
                    "Documents": [{
                      "Url": "https://example.test/news",
                      "Title": "最新消息",
                      "Snippet": [{ "Type": "text", "Text": "今天__CONTROL__发布" }],
                      "DocumentInfo": { "PublishTime": "2026-07-27" },
                      "HostInfo": { "Hostname": "example.test" }
                    }]
                  }
                }
                """.Replace("__CONTROL__", "\u0001", StringComparison.Ordinal);
                return JsonResponse(payload);
            }
            modelBody = body;
            return JsonResponse("""{ "choices": [ { "message": { "content": "联网答案" } } ] }""");
        });
        var client = new AiChatClient(
            settings, new AiProviderProfileStore(_file), _secrets, new HttpClient(handler));

        AiChatResult result = await client.SendAsync("今天有什么新消息？", CancellationToken.None);

        Assert.Equal(2, handler.Calls);
        Assert.Equal("豆包搜索", result.SearchProvider);
        Assert.Contains("\"Query\":\"今天有什么新消息？\"", searchBody);
        Assert.Contains("\"MaxImageCountPerDoc\":1", searchBody);
        Assert.Contains("https://example.test/news", modelBody);
        Assert.Contains("今天 发布", modelBody);
        Assert.Contains("用 [序号] 标注来源", modelBody);
    }

    [Fact]
    public async Task SendAsync_WhenDoubaoReturnsBusinessError_FallsBackToPlainChat()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Ai.CloudEnabled = true;
        settings.Ai.AskWebSearchEnabled = true;
        AddDefaultProfile("sk-model");
        _secrets.SetSecret(WebSearchBackends.SecretName(WebSearchBackends.Doubao), "bad-search-key");
        var handler = new CaptureHandler(request =>
            request.RequestUri!.Host == "open.feedcoopapi.com"
                ? JsonResponse("""
                    {
                      "ResponseMetadata": {
                        "Error": { "Code": "700901", "Message": "invalid_api_key" }
                      }
                    }
                    """)
                : JsonResponse("""{ "choices": [ { "message": { "content": "普通答案" } } ] }"""));
        var client = new AiChatClient(
            settings, new AiProviderProfileStore(_file), _secrets, new HttpClient(handler));

        AiChatResult result = await client.SendAsync("继续回答", CancellationToken.None);

        Assert.Equal(2, handler.Calls);
        Assert.Null(result.SearchProvider);
        Assert.Equal("普通答案", result.Text);
    }

    [Fact]
    public async Task SendAsync_WithPerplexitySearch_UsesDedicatedEndpoint()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Ai.CloudEnabled = true;
        settings.Ai.AskWebSearchEnabled = true;
        settings.Ai.WebSearchBackend = WebSearchBackends.Perplexity;
        AddDefaultProfile("sk-model");
        _secrets.SetSecret(WebSearchBackends.SecretName(WebSearchBackends.Perplexity), "pplx-test");
        string searchBody = "";
        var handler = new CaptureHandler(request =>
        {
            if (request.RequestUri!.Host == "api.perplexity.ai")
            {
                searchBody = request.Content!.ReadAsStringAsync().GetAwaiter().GetResult();
                return JsonResponse("""
                    {
                      "results": [{
                        "url": "https://www.example.com/report",
                        "title": "Report",
                        "snippet": "Fresh facts",
                        "date": "2026-07-27"
                      }]
                    }
                    """);
            }
            return JsonResponse("""{ "choices": [ { "message": { "content": "answer" } } ] }""");
        });
        var client = new AiChatClient(
            settings, new AiProviderProfileStore(_file), _secrets, new HttpClient(handler));

        AiChatResult result = await client.SendAsync("latest report", CancellationToken.None);

        Assert.Equal("Perplexity", result.SearchProvider);
        Assert.Contains("\"search_context_size\":\"high\"", searchBody);
        Assert.Contains("\"max_results\":5", searchBody);
    }

    private AiProviderProfile AddDefaultProfile(string key)
    {
        var service = new AiProviderProfileService(new AiProviderProfileStore(_file), _secrets);
        return service.AddProfile(new AiProviderProfileDraft
        {
            DisplayName = "DeepSeek",
            Endpoint = "https://api.deepseek.com/v1/chat/completions",
            Model = "deepseek-chat",
            ApiKey = key,
            MakeDefault = true,
        });
    }

    private static HttpResponseMessage JsonResponse(string json) =>
        new(HttpStatusCode.OK)
        {
            Content = new StringContent(json, Encoding.UTF8, "application/json"),
        };

    private sealed class CaptureHandler : HttpMessageHandler
    {
        private readonly Func<HttpRequestMessage, HttpResponseMessage> _send;

        public int Calls { get; private set; }

        public CaptureHandler(Func<HttpRequestMessage, HttpResponseMessage> send)
        {
            _send = send;
        }

        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            Calls++;
            return Task.FromResult(_send(request));
        }
    }

    private sealed class FakeSecretStore : ISecretStore
    {
        private readonly Dictionary<string, string> _values = new(StringComparer.Ordinal);

        public void SetSecret(string name, string value) => _values[name] = value;

        public string? GetSecret(string name) => _values.TryGetValue(name, out string? value) ? value : null;

        public void DeleteSecret(string name) => _values.Remove(name);

        public void Clear() => _values.Clear();
    }
}
