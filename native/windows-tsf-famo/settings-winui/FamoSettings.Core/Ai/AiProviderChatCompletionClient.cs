using System.Net.Http.Headers;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;

namespace Famo.Settings.Core.Ai;

internal sealed record AiProviderChatMessage(string Role, string Content);

internal sealed record AiProviderChatCompletionResult(string Text, string ProviderId, string Model);

internal sealed class AiProviderChatCompletionClient
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    private readonly AiProviderProfileStore _profiles;
    private readonly ISecretStore _secrets;
    private readonly HttpClient _http;

    public AiProviderChatCompletionClient(
        AiProviderProfileStore profiles,
        ISecretStore secrets,
        HttpClient? http = null)
    {
        _profiles = profiles;
        _secrets = secrets;
        _http = http ?? new HttpClient { Timeout = TimeSpan.FromSeconds(45) };
    }

    public async Task<AiProviderChatCompletionResult> SendAsync(
        IReadOnlyList<AiProviderChatMessage> messages,
        CancellationToken cancellationToken,
        bool jsonObjectResponse = false)
    {
        if (messages.Count == 0)
        {
            throw new InvalidOperationException("AI 请求缺少消息。");
        }

        AiProviderProfile profile = _profiles.DefaultProfile()
            ?? throw new InvalidOperationException("尚未配置 AI 供应商。");
        string apiKey = _secrets.GetSecret(profile.SecretName)
            ?? throw new InvalidOperationException("默认 AI 供应商缺少 API Key，请先在设置中重新保存。");
        Uri endpoint = ParseEndpoint(profile.Endpoint);

        using var request = new HttpRequestMessage(HttpMethod.Post, endpoint);
        request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", apiKey);
        request.Content = new StringContent(
            BuildRequestJson(profile, messages, jsonObjectResponse),
            Encoding.UTF8,
            "application/json");

        using HttpResponseMessage response = await _http.SendAsync(request, cancellationToken);
        string json = await response.Content.ReadAsStringAsync(cancellationToken);
        if (!response.IsSuccessStatusCode)
        {
            throw new InvalidOperationException($"AI 请求失败：HTTP {(int)response.StatusCode} {ExtractErrorMessage(json)}".Trim());
        }

        return new AiProviderChatCompletionResult(ParseAssistantText(json), profile.Id, profile.Model);
    }

    private static string BuildRequestJson(
        AiProviderProfile profile,
        IReadOnlyList<AiProviderChatMessage> messages,
        bool jsonObjectResponse)
    {
        var wireMessages = messages
            .Select(m => new
            {
                role = m.Role,
                content = m.Content,
            })
            .ToArray();

        var body = new Dictionary<string, object?>(StringComparer.Ordinal)
        {
            ["model"] = profile.Model,
            ["messages"] = wireMessages,
            ["stream"] = false,
        };

        if (jsonObjectResponse)
        {
            body["response_format"] = new { type = "json_object" };
        }

        ApplyThinkingOff(body, profile.Model);

        return JsonSerializer.Serialize(body, JsonOptions);
    }

    /// <summary>
    /// 全技能一律关思考的单一策略点。qwen3.6-flash / deepseek 这类混合思考模型默认开思考，
    /// 短输出前先烧几百上千 reasoning token。此前 Windows 侧完全没有这个开关，所有 AI 技能
    /// 都带着思考在跑。
    ///
    /// 2026-07-20 实盘 A/B（探针 macOS 主线仓 scripts/Test-FamoAISkills.ps1 ± -ThinkingOff，
    /// 打真实百炼 qwen3.6-flash）：提示词优化 24.4s → 2.5s、任意提问第二轮 9.4s → 1.7s，
    /// 而四要素补齐 / 专名保留 / 多轮承接 / 多版本区分度断言逐条不变——质量零回退。
    ///
    /// 只发给认得该字段的供应商：别家一个字段都不加，请求体逐字节不变。DashScope 对未知
    /// 字段是会拒的，所以绝不能无差别下发。
    /// </summary>
    private static void ApplyThinkingOff(IDictionary<string, object?> body, string model)
    {
        if (model.StartsWith("deepseek", StringComparison.OrdinalIgnoreCase))
        {
            body["thinking"] = new { type = "disabled" };
        }
        else if (model.StartsWith("qwen", StringComparison.OrdinalIgnoreCase))
        {
            body["enable_thinking"] = false;
        }
    }

    private static string ParseAssistantText(string json)
    {
        try
        {
            using JsonDocument doc = JsonDocument.Parse(json);
            JsonElement first = doc.RootElement.GetProperty("choices")[0];
            if (first.TryGetProperty("message", out JsonElement message)
                && message.TryGetProperty("content", out JsonElement content)
                && content.ValueKind == JsonValueKind.String)
            {
                string? text = content.GetString();
                if (!string.IsNullOrWhiteSpace(text)) return text.Trim();
            }
        }
        catch (Exception ex) when (ex is JsonException or KeyNotFoundException or InvalidOperationException)
        {
            throw new InvalidOperationException("AI 响应格式无法解析。", ex);
        }

        throw new InvalidOperationException("AI 响应为空。");
    }

    private static string ExtractErrorMessage(string json)
    {
        if (string.IsNullOrWhiteSpace(json)) return "";

        try
        {
            using JsonDocument doc = JsonDocument.Parse(json);
            if (doc.RootElement.TryGetProperty("error", out JsonElement error)
                && error.TryGetProperty("message", out JsonElement message)
                && message.ValueKind == JsonValueKind.String)
            {
                return message.GetString() ?? "";
            }
        }
        catch (JsonException)
        {
            return json.Length <= 160 ? json : json[..160];
        }

        return "";
    }

    private static Uri ParseEndpoint(string endpoint)
    {
        if (!Uri.TryCreate(endpoint.Trim(), UriKind.Absolute, out Uri? uri))
        {
            throw new InvalidOperationException("AI 供应商 Endpoint 无效。");
        }
        if (uri.Scheme == Uri.UriSchemeHttps)
        {
            return uri;
        }
        if (uri.Scheme == Uri.UriSchemeHttp
            && (uri.IsLoopback || string.Equals(uri.Host, "localhost", StringComparison.OrdinalIgnoreCase)))
        {
            return uri;
        }

        throw new InvalidOperationException("AI 供应商 Endpoint 必须是 HTTPS，或本机 HTTP 调试地址。");
    }
}
