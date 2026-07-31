using System.Net;
using System.Text;
using System.Text.Json;
using Famo.Settings.Core;
using Famo.Settings.Core.Ai;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class QwenResponsesApiContractTests : IDisposable
{
    private const string WorkspaceId = "llm-test-workspace";
    private readonly string _dir;
    private readonly string _file;
    private readonly FakeSecretStore _secrets = new();

    public QwenResponsesApiContractTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "famo-qwen-responses-" + Guid.NewGuid().ToString("N"));
        _file = Path.Combine(_dir, "ai-providers.json");
    }

    public void Dispose()
    {
        if (Directory.Exists(_dir)) Directory.Delete(_dir, recursive: true);
    }

    [Fact]
    public void BuildBeijingEndpoint_UsesTheUserWorkspaceId()
    {
        Assert.Equal(
            $"https://{WorkspaceId}.cn-beijing.maas.aliyuncs.com/compatible-mode/v1/responses",
            QwenResponsesApi.BuildBeijingEndpoint(WorkspaceId));
    }

    [Theory]
    [InlineData("")]
    [InlineData("workspace.example.com")]
    [InlineData("workspace/path")]
    [InlineData("{WorkspaceId}")]
    public void BuildBeijingEndpoint_RejectsValuesThatCannotBeAWorkspaceHostLabel(string value)
    {
        Assert.Throws<InvalidDataException>(() => QwenResponsesApi.BuildBeijingEndpoint(value));
    }

    [Theory]
    [InlineData("https://example.test/compatible-mode/v1/responses")]
    [InlineData("https://workspace.other.cn-beijing.maas.aliyuncs.com/compatible-mode/v1/responses")]
    [InlineData("https://workspace.cn-beijing.maas.aliyuncs.com/compatible-mode/v1/responses?debug=true")]
    [InlineData("https://workspace.cn-beijing.maas.aliyuncs.com/compatible-mode/v1/Responses")]
    [InlineData("http://workspace.cn-beijing.maas.aliyuncs.com/compatible-mode/v1/responses")]
    public void IsResponsesEndpoint_RejectsAnythingOutsideTheExactWorkspaceRoute(string value)
    {
        Assert.False(QwenResponsesApi.IsResponsesEndpoint(new Uri(value)));
    }

    [Fact]
    public async Task SendAsync_UsesResponsesWireShapeAndParsesOutputText()
    {
        FamoSettings settings = QwenSettings();
        HttpRequestMessage? captured = null;
        string capturedBody = "";
        var http = new HttpClient(new CaptureHandler(request =>
        {
            captured = request;
            capturedBody = request.Content!.ReadAsStringAsync().GetAwaiter().GetResult();
            return JsonResponse(
                """
                {
                  "id": "resp_test",
                  "object": "response",
                  "status": "completed",
                  "output": [
                    {
                      "id": "msg_test",
                      "type": "message",
                      "role": "assistant",
                      "status": "completed",
                      "content": [
                        { "type": "output_text", "text": "千问响应", "annotations": [] }
                      ]
                    }
                  ]
                }
                """);
        }));

        var client = new AiChatClient(
            settings, new AiProviderProfileStore(_file), _secrets, http);

        AiChatResult result = await client.SendAsync("你好", CancellationToken.None);

        Assert.Equal("千问响应", result.Text);
        Assert.NotNull(captured);
        Assert.Equal(HttpMethod.Post, captured.Method);
        Assert.Equal(QwenResponsesApi.BuildBeijingEndpoint(WorkspaceId), captured.RequestUri!.ToString());
        Assert.Equal("Bearer", captured.Headers.Authorization!.Scheme);
        Assert.Equal("sk-qwen-test", captured.Headers.Authorization.Parameter);

        using JsonDocument body = JsonDocument.Parse(capturedBody);
        JsonElement root = body.RootElement;
        Assert.Equal("qwen3.6-flash", root.GetProperty("model").GetString());
        Assert.Equal(JsonValueKind.Array, root.GetProperty("input").ValueKind);
        Assert.False(root.GetProperty("stream").GetBoolean());
        Assert.False(root.GetProperty("store").GetBoolean());
        Assert.Equal("none", root.GetProperty("reasoning").GetProperty("effort").GetString());
        Assert.False(root.TryGetProperty("messages", out _));
        Assert.False(root.TryGetProperty("enable_thinking", out _));
        Assert.False(root.TryGetProperty("response_format", out _));
    }

    [Fact]
    public async Task SendAsync_WithQwenSearch_UsesTheBuiltInWebSearchToolAndNoSearchCredential()
    {
        FamoSettings settings = QwenSettings();
        settings.Ai.AskWebSearchEnabled = true;
        settings.Ai.WebSearchBackend = WebSearchBackends.Qwen;
        string capturedBody = "";
        var http = new HttpClient(new CaptureHandler(request =>
        {
            capturedBody = request.Content!.ReadAsStringAsync().GetAwaiter().GetResult();
            return JsonResponse(
                """
                {
                  "id": "resp_search_test",
                  "object": "response",
                  "status": "completed",
                  "output": [
                    {
                      "id": "search_test",
                      "type": "web_search_call",
                      "status": "completed",
                      "action": {
                        "type": "search",
                        "query": "今日新闻",
                        "sources": [{ "type": "url", "url": "https://example.test/news" }]
                      }
                    },
                    {
                      "id": "msg_search_test",
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
        Assert.Equal(WebSearchBackends.DisplayName(WebSearchBackends.Qwen), result.SearchProvider);
        using JsonDocument body = JsonDocument.Parse(capturedBody);
        JsonElement root = body.RootElement;
        JsonElement tool = Assert.Single(root.GetProperty("tools").EnumerateArray());
        Assert.Equal("web_search", tool.GetProperty("type").GetString());
        Assert.Equal("required", root.GetProperty("tool_choice").GetString());
        Assert.False(root.GetProperty("store").GetBoolean());
        Assert.Null(_secrets.GetSecret(WebSearchBackends.SecretName(WebSearchBackends.Qwen)));
    }

    [Fact]
    public async Task SendAsync_WithJsonObjectResponse_AddsAJsonOnlyInstructionAndJoinsAllOutputParts()
    {
        FamoSettings settings = QwenSettings();
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
        JsonElement input = body.RootElement.GetProperty("input");
        Assert.Equal("developer", input[0].GetProperty("role").GetString());
        Assert.Contains(
            "valid JSON object",
            input[0].GetProperty("content").GetString());
        Assert.Equal("user", input[1].GetProperty("role").GetString());
        Assert.False(body.RootElement.TryGetProperty("response_format", out _));
    }

    [Fact]
    public async Task SendAsync_WithJsonObjectResponse_RejectsANonObjectResponse()
    {
        FamoSettings settings = QwenSettings();
        var http = new HttpClient(new CaptureHandler(_ => JsonResponse(
            """
            {
              "status": "completed",
              "output": [
                {
                  "type": "message",
                  "role": "assistant",
                  "status": "completed",
                  "content": [
                    { "type": "output_text", "text": "[]", "annotations": [] }
                  ]
                }
              ]
            }
            """)));
        var client = new AiProviderChatCompletionClient(
            new AiProviderProfileStore(_file), _secrets, http);

        InvalidOperationException error = await Assert.ThrowsAsync<InvalidOperationException>(
            () => client.SendAsync(
                [new AiProviderChatMessage("user", "请返回 JSON。")],
                CancellationToken.None,
                jsonObjectResponse: true));

        Assert.Contains("JSON 对象", error.Message);
    }

    private FamoSettings QwenSettings()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Ai.CloudEnabled = true;
        var service = new AiProviderProfileService(new AiProviderProfileStore(_file), _secrets);
        service.AddProfile(new AiProviderProfileDraft
        {
            DisplayName = "阿里云百炼",
            Endpoint = QwenResponsesApi.BuildBeijingEndpoint(WorkspaceId),
            Model = "qwen3.6-flash",
            ApiKey = "sk-qwen-test",
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

        public string? GetSecret(string name) => _values.TryGetValue(name, out string? v) ? v : null;

        public void DeleteSecret(string name) => _values.Remove(name);
    }
}
