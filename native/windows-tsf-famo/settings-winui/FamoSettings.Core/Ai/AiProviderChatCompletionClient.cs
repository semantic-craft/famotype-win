using System.Net.Http.Headers;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using Famo.Settings.Core;

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

    internal void EnsureReady() => _ = ResolveDefault();

    public async Task<AiProviderChatCompletionResult> SendAsync(
        IReadOnlyList<AiProviderChatMessage> messages,
        CancellationToken cancellationToken,
        bool jsonObjectResponse = false)
    {
        if (messages.Count == 0)
        {
            throw new InvalidOperationException("AI 请求缺少消息。");
        }

        var (profile, apiKey, endpoint) = ResolveDefault();

        using var request = new HttpRequestMessage(HttpMethod.Post, endpoint);
        ApplyAuth(request, profile, apiKey);
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

    private (AiProviderProfile Profile, string ApiKey, Uri Endpoint) ResolveDefault()
    {
        AiProviderProfile profile = _profiles.DefaultProfile()
            ?? throw new InvalidOperationException("尚未配置 AI 供应商。");
        if (string.IsNullOrWhiteSpace(profile.Model))
            throw new InvalidOperationException("默认 AI 供应商缺少模型 ID，请先在设置中补全。");
        string? apiKey = _secrets.GetSecret(profile.SecretName);
        if (string.IsNullOrWhiteSpace(apiKey))
            throw new InvalidOperationException("默认 AI 供应商缺少 API Key，请先在设置中重新保存。");
        return (profile, apiKey, ParseEndpoint(profile.Endpoint));
    }

    public async Task<AiProviderChatCompletionResult> SendSourceVerificationAsync(
        IReadOnlyList<AiProviderChatMessage> messages,
        CancellationToken cancellationToken)
    {
        if (messages.Count == 0)
        {
            throw new InvalidOperationException("AI 请求缺少消息。");
        }

        AiProviderProfile profile = _profiles.DefaultProfile()
            ?? throw new InvalidOperationException("尚未配置 AI 供应商。");
        if (string.IsNullOrWhiteSpace(profile.Model))
        {
            throw new InvalidOperationException("默认 AI 供应商缺少模型 ID，请先在设置中补全。");
        }
        string provider = InferProvider(profile);
        if (provider is not ("qwen" or "mimo" or "doubao" or "openai"))
        {
            throw new InvalidOperationException(
                "当前默认供应商不支持返回来源的联网核验，请切换到阿里云百炼、小米 MiMo、火山引擎豆包或 OpenAI。");
        }

        string apiKey = _secrets.GetSecret(profile.SecretName)
            ?? throw new InvalidOperationException("默认 AI 供应商缺少 API Key，请先在设置中重新保存。");
        Uri endpoint = SourceVerificationEndpoint(ParseEndpoint(profile.Endpoint), provider);

        using var request = new HttpRequestMessage(HttpMethod.Post, endpoint);
        ApplyAuth(request, profile, apiKey);
        request.Content = new StringContent(
            BuildSourceVerificationRequestJson(profile, provider, messages),
            Encoding.UTF8,
            "application/json");

        using HttpResponseMessage response = await _http.SendAsync(request, cancellationToken);
        string json = await response.Content.ReadAsStringAsync(cancellationToken);
        if (!response.IsSuccessStatusCode)
        {
            throw new InvalidOperationException($"来源核验请求失败：HTTP {(int)response.StatusCode} {ExtractErrorMessage(json)}".Trim());
        }

        return new AiProviderChatCompletionResult(RenderSourceVerification(json), profile.Id, profile.Model);
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

    private static string BuildSourceVerificationRequestJson(
        AiProviderProfile profile,
        string provider,
        IReadOnlyList<AiProviderChatMessage> messages)
    {
        object[] wireMessages = messages
            .Select(m => (object)new { role = m.Role, content = m.Content })
            .ToArray();

        if (provider == "qwen")
        {
            return JsonSerializer.Serialize(new
            {
                model = "qwen-plus",
                input = new { messages = wireMessages },
                parameters = new
                {
                    enable_search = true,
                    search_options = new
                    {
                        forced_search = true,
                        search_strategy = "max",
                        enable_source = true,
                    },
                    result_format = "message",
                },
            }, JsonOptions);
        }

        if (provider == "mimo")
        {
            return JsonSerializer.Serialize(new
            {
                model = profile.Model,
                messages = wireMessages,
                temperature = 0.2,
                stream = false,
                thinking = new { type = "disabled" },
                tool_choice = "auto",
                tools = new[]
                {
                    new { type = "web_search", max_keyword = 3, force_search = true, limit = 2 },
                },
            }, JsonOptions);
        }

        object[] input = messages.Select(m => (object)new
        {
            role = m.Role,
            content = new[] { new { type = "input_text", text = m.Content } },
        }).ToArray();
        var body = new Dictionary<string, object?>(StringComparer.Ordinal)
        {
            ["model"] = profile.Model,
            ["input"] = input,
            ["stream"] = false,
            ["tools"] = provider == "openai"
                ? new object[] { new { type = "web_search" } }
                : new object[] { new { type = "web_search", max_keyword = 2, limit = 3 } },
        };
        if (provider == "doubao")
        {
            body["thinking"] = new { type = "disabled" };
            body["max_tool_calls"] = 3;
        }
        return JsonSerializer.Serialize(body, JsonOptions);
    }

    private static void ApplyAuth(HttpRequestMessage request, AiProviderProfile profile, string apiKey)
    {
        if (InferProvider(profile) == "mimo")
        {
            request.Headers.Add("api-key", apiKey);
            return;
        }
        request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", apiKey);
    }

    private static string InferProvider(AiProviderProfile profile)
    {
        string host = Uri.TryCreate(profile.Endpoint, UriKind.Absolute, out Uri? endpoint)
            ? endpoint.Host.ToLowerInvariant()
            : "";
        string name = profile.DisplayName.ToLowerInvariant();
        if (host.Contains("dashscope") || host.Contains("aliyuncs") || name.Contains("百炼") || name.Contains("qwen")) return "qwen";
        if (host.Contains("xiaomimimo") || name.Contains("mimo")) return "mimo";
        if (host.Contains("volces") || host.Contains("volcengine") || name.Contains("豆包")) return "doubao";
        if (host == "api.openai.com" || name.Contains("openai")) return "openai";
        return "unsupported";
    }

    private static Uri SourceVerificationEndpoint(Uri endpoint, string provider)
    {
        var builder = new UriBuilder(endpoint) { Query = "", Fragment = "" };
        if (provider == "qwen")
        {
            builder.Path = "/api/v1/services/aigc/text-generation/generation";
            return builder.Uri;
        }
        if (provider is "doubao" or "openai")
        {
            const string chatSuffix = "/chat/completions";
            string path = builder.Path.TrimEnd('/');
            builder.Path = path.EndsWith(chatSuffix, StringComparison.OrdinalIgnoreCase)
                ? path[..^chatSuffix.Length] + "/responses"
                : path + "/responses";
        }
        return builder.Uri;
    }

    private static string RenderSourceVerification(string json)
    {
        try
        {
            using JsonDocument doc = JsonDocument.Parse(json);
            string content = FindAssistantContent(doc.RootElement).Trim();
            var sources = new List<(string Title, string Url)>();
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            CollectSources(doc.RootElement, sources, seen);

            if (sources.Count > 0)
            {
                string sourceLines = string.Join("\n", sources.Take(8).Select(source =>
                    $"- {(string.IsNullOrWhiteSpace(source.Title) ? source.Url : source.Title)}\n  {source.Url}"));
                string summary = string.IsNullOrWhiteSpace(content)
                    ? "已通过联网检索取得来源 URL，请人工复核来源内容与原文主张是否完全对应。"
                    : content;
                return $"已找到可核来源\n\n{summary}\n\n来源：\n{sourceLines}";
            }
            if (!string.IsNullOrWhiteSpace(content))
            {
                return "需要人工确认\n\n模型返回了联网核验摘要，但没有返回可抽取的来源 URL。不要把下面内容当作已完成核验：\n\n" + content;
            }
            return "未找到可核来源。注意：这不等于事实不存在。";
        }
        catch (JsonException ex)
        {
            throw new InvalidOperationException("来源核验响应格式无法解析。", ex);
        }
    }

    private static string FindAssistantContent(JsonElement element)
    {
        if (element.ValueKind == JsonValueKind.Object)
        {
            if (element.TryGetProperty("choices", out JsonElement choices)
                && choices.ValueKind == JsonValueKind.Array
                && choices.GetArrayLength() > 0
                && choices[0].TryGetProperty("message", out JsonElement message)
                && message.TryGetProperty("content", out JsonElement content)
                && content.ValueKind == JsonValueKind.String)
            {
                return content.GetString() ?? "";
            }
            if (element.TryGetProperty("output", out JsonElement output))
            {
                if (output.ValueKind == JsonValueKind.Object)
                {
                    if (output.TryGetProperty("text", out JsonElement text) && text.ValueKind == JsonValueKind.String)
                    {
                        return text.GetString() ?? "";
                    }
                    string nested = FindAssistantContent(output);
                    if (nested.Length > 0) return nested;
                }
                else if (output.ValueKind == JsonValueKind.Array)
                {
                    foreach (JsonElement item in output.EnumerateArray())
                    {
                        string nested = FindAssistantContent(item);
                        if (nested.Length > 0) return nested;
                    }
                }
            }
            if (element.TryGetProperty("content", out JsonElement parts) && parts.ValueKind == JsonValueKind.Array)
            {
                foreach (JsonElement part in parts.EnumerateArray())
                {
                    if (part.TryGetProperty("text", out JsonElement text) && text.ValueKind == JsonValueKind.String)
                    {
                        return text.GetString() ?? "";
                    }
                }
            }
        }
        return "";
    }

    private static void CollectSources(
        JsonElement element,
        List<(string Title, string Url)> sources,
        HashSet<string> seen)
    {
        if (sources.Count >= 12) return;
        if (element.ValueKind == JsonValueKind.Object)
        {
            string url = FirstStringProperty(element, "url", "link", "source_url", "uri");
            if (Uri.TryCreate(url, UriKind.Absolute, out Uri? uri)
                && uri.Scheme is "http" or "https"
                && seen.Add(uri.AbsoluteUri))
            {
                sources.Add((FirstStringProperty(element, "title", "name", "site_name"), uri.AbsoluteUri));
            }
            foreach (JsonProperty property in element.EnumerateObject())
            {
                CollectSources(property.Value, sources, seen);
            }
        }
        else if (element.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement item in element.EnumerateArray())
            {
                CollectSources(item, sources, seen);
            }
        }
    }

    private static string FirstStringProperty(JsonElement element, params string[] names)
    {
        foreach (string name in names)
        {
            if (element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String)
            {
                return value.GetString() ?? "";
            }
        }
        return "";
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
            return TextElementTruncator.Truncate(json, 160);
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
