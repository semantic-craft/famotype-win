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
        bool jsonObjectResponse = false,
        bool useNativeWebSearch = false)
    {
        if (messages.Count == 0)
        {
            throw new InvalidOperationException("AI 请求缺少消息。");
        }

        var (profile, apiKey, endpoint) = ResolveDefault();
        string provider = InferProvider(profile);
        if (useNativeWebSearch && provider != "qwen")
        {
            throw new InvalidOperationException(
                "阿里云百炼内置搜索要求默认 AI 供应商为阿里云百炼。");
        }

        using var request = new HttpRequestMessage(HttpMethod.Post, endpoint);
        ApplyAuth(request, profile, apiKey);
        request.Content = new StringContent(
            BuildRequestJson(
                profile, provider, messages, jsonObjectResponse, useNativeWebSearch),
            Encoding.UTF8,
            "application/json");

        using HttpResponseMessage response = await _http.SendAsync(
            request, HttpCompletionOption.ResponseHeadersRead, cancellationToken);
        string json = await ReadResponseAsync(response, cancellationToken);
        if (!response.IsSuccessStatusCode)
        {
            throw new InvalidOperationException($"AI 请求失败：HTTP {(int)response.StatusCode} {ExtractErrorMessage(json)}".Trim());
        }

        string text = useNativeWebSearch
            ? ParseAssistantTextWithSources(json)
            : ParseAssistantText(json);
        if (provider == "qwen" && jsonObjectResponse)
        {
            EnsureJsonObjectResponse(text);
        }
        return new AiProviderChatCompletionResult(text, profile.Id, profile.Model);
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
        Uri endpoint = ParseEndpoint(profile.Endpoint);
        if (InferProvider(profile) == "qwen"
            && !QwenResponsesApi.IsResponsesEndpoint(endpoint))
        {
            throw new InvalidOperationException(
                "千问供应商必须使用包含 Workspace ID 的 Responses API 地址；请删除旧配置后重新保存。");
        }
        return (profile, apiKey, endpoint);
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
        Uri configuredEndpoint = ParseEndpoint(profile.Endpoint);
        if (provider == "qwen"
            && !QwenResponsesApi.IsResponsesEndpoint(configuredEndpoint))
        {
            throw new InvalidOperationException(
                "千问供应商必须使用包含 Workspace ID 的 Responses API 地址；请删除旧配置后重新保存。");
        }
        Uri endpoint = SourceVerificationEndpoint(configuredEndpoint, provider);

        using var request = new HttpRequestMessage(HttpMethod.Post, endpoint);
        ApplyAuth(request, profile, apiKey);
        request.Content = new StringContent(
            BuildSourceVerificationRequestJson(profile, provider, messages),
            Encoding.UTF8,
            "application/json");

        using HttpResponseMessage response = await _http.SendAsync(
            request, HttpCompletionOption.ResponseHeadersRead, cancellationToken);
        string json = await ReadResponseAsync(response, cancellationToken);
        if (!response.IsSuccessStatusCode)
        {
            throw new InvalidOperationException($"来源核验请求失败：HTTP {(int)response.StatusCode} {ExtractErrorMessage(json)}".Trim());
        }

        return new AiProviderChatCompletionResult(RenderSourceVerification(json), profile.Id, profile.Model);
    }

    private static async Task<string> ReadResponseAsync(
        HttpResponseMessage response,
        CancellationToken cancellationToken)
    {
        try
        {
            return await BoundedHttpContent.ReadUtf8Async(
                response.Content, cancellationToken);
        }
        catch (InvalidDataException ex)
        {
            throw new InvalidOperationException(
                "AI 响应过大，已停止读取。", ex);
        }
    }

    private static string BuildRequestJson(
        AiProviderProfile profile,
        string provider,
        IReadOnlyList<AiProviderChatMessage> messages,
        bool jsonObjectResponse,
        bool useNativeWebSearch)
    {
        object[] wireMessages = messages
            .Select(m => (object)new
            {
                role = m.Role,
                content = m.Content,
            })
            .ToArray();

        if (provider == "qwen")
        {
            object[] responseInput = jsonObjectResponse
                ?
                [
                    new
                    {
                        role = "developer",
                        content =
                            "Return exactly one valid JSON object and nothing else. "
                            + "Do not use Markdown or code fences.",
                    },
                    .. wireMessages,
                ]
                : wireMessages;
            var responseBody = new Dictionary<string, object?>(StringComparer.Ordinal)
            {
                ["model"] = profile.Model,
                ["input"] = responseInput,
                ["stream"] = false,
                ["store"] = false,
                ["reasoning"] = new { effort = "none" },
            };
            if (useNativeWebSearch)
            {
                responseBody["tools"] = new object[] { new { type = "web_search" } };
                responseBody["tool_choice"] = "required";
            }
            return JsonSerializer.Serialize(responseBody, JsonOptions);
        }

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
            return JsonSerializer.Serialize(new Dictionary<string, object?>
            {
                ["model"] = profile.Model,
                ["input"] = wireMessages,
                ["stream"] = false,
                ["store"] = false,
                ["reasoning"] = new { effort = "none" },
                ["tools"] = new object[] { new { type = "web_search" } },
                ["tool_choice"] = "required",
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
                string sourceLines = string.Join(
                    "\n",
                    sources.Take(8).Select(FormatSource));
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
        if (element.ValueKind != JsonValueKind.Object)
        {
            return "";
        }

        if (element.TryGetProperty("choices", out JsonElement choices)
            && choices.ValueKind == JsonValueKind.Array
            && choices.GetArrayLength() > 0
            && choices[0].TryGetProperty("message", out JsonElement message)
            && message.TryGetProperty("content", out JsonElement content)
            && content.ValueKind == JsonValueKind.String)
        {
            return content.GetString() ?? "";
        }
        if (!element.TryGetProperty("output", out JsonElement output))
        {
            return "";
        }

        if (output.ValueKind == JsonValueKind.Object)
        {
            if (output.TryGetProperty("text", out JsonElement text)
                && text.ValueKind == JsonValueKind.String)
            {
                return text.GetString() ?? "";
            }
            return FindAssistantContent(output);
        }
        if (output.ValueKind != JsonValueKind.Array)
        {
            return "";
        }

        var parts = new List<string>();
        foreach (JsonElement item in output.EnumerateArray())
        {
            if (item.ValueKind != JsonValueKind.Object)
            {
                continue;
            }
            if (item.TryGetProperty("type", out JsonElement itemType)
                && itemType.ValueKind == JsonValueKind.String
                && itemType.GetString() != "message")
            {
                continue;
            }
            if (!item.TryGetProperty("content", out JsonElement itemContent)
                || itemContent.ValueKind != JsonValueKind.Array)
            {
                continue;
            }
            foreach (JsonElement part in itemContent.EnumerateArray())
            {
                if (part.ValueKind != JsonValueKind.Object)
                {
                    continue;
                }
                if (part.TryGetProperty("type", out JsonElement partType)
                    && partType.ValueKind == JsonValueKind.String
                    && partType.GetString() != "output_text")
                {
                    continue;
                }
                if (part.TryGetProperty("text", out JsonElement partText)
                    && partText.ValueKind == JsonValueKind.String)
                {
                    parts.Add(partText.GetString() ?? "");
                }
            }
        }
        return string.Concat(parts);
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
    /// Chat Completions 供应商的关思考策略点。千问已迁移到 Responses API，
    /// 由其请求体使用官方推荐的 reasoning.effort:none。
    ///
    /// 2026-07-20 实盘 A/B（探针 macOS 主线仓 scripts/Test-FamoAISkills.ps1 ± -ThinkingOff，
    /// 打真实百炼 qwen3.6-flash）：提示词优化 24.4s → 2.5s、任意提问第二轮 9.4s → 1.7s，
    /// 而四要素补齐 / 专名保留 / 多轮承接 / 多版本区分度断言逐条不变——质量零回退。
    ///
    /// 只发给认得该字段的供应商：别家一个字段都不加，请求体逐字节不变。
    /// </summary>
    private static void ApplyThinkingOff(IDictionary<string, object?> body, string model)
    {
        if (model.StartsWith("deepseek", StringComparison.OrdinalIgnoreCase))
        {
            body["thinking"] = new { type = "disabled" };
        }
    }

    private static string ParseAssistantText(string json)
    {
        try
        {
            using JsonDocument doc = JsonDocument.Parse(json);
            string text = FindAssistantContent(doc.RootElement);
            if (!string.IsNullOrWhiteSpace(text)) return text.Trim();
        }
        catch (Exception ex) when (ex is JsonException or KeyNotFoundException or InvalidOperationException)
        {
            throw new InvalidOperationException("AI 响应格式无法解析。", ex);
        }

        throw new InvalidOperationException("AI 响应为空。");
    }

    private static string ParseAssistantTextWithSources(string json)
    {
        try
        {
            using JsonDocument doc = JsonDocument.Parse(json);
            string text = FindAssistantContent(doc.RootElement).Trim();
            if (text.Length == 0)
            {
                throw new InvalidOperationException("AI 响应为空。");
            }

            var sources = new List<(string Title, string Url)>();
            CollectSources(
                doc.RootElement,
                sources,
                new HashSet<string>(StringComparer.OrdinalIgnoreCase));
            if (sources.Count == 0) return text;

            string sourceLines = string.Join(
                "\n",
                sources.Take(8).Select(FormatSource));
            return $"{text}\n\n来源：\n{sourceLines}";
        }
        catch (JsonException ex)
        {
            throw new InvalidOperationException("AI 响应格式无法解析。", ex);
        }
    }

    private static string FormatSource((string Title, string Url) source) =>
        string.IsNullOrWhiteSpace(source.Title)
            ? $"- {source.Url}"
            : $"- {source.Title}\n  {source.Url}";

    private static void EnsureJsonObjectResponse(string text)
    {
        try
        {
            using JsonDocument document = JsonDocument.Parse(text);
            if (document.RootElement.ValueKind != JsonValueKind.Object)
            {
                throw new InvalidOperationException("千问响应不是 JSON 对象。");
            }
        }
        catch (JsonException ex)
        {
            throw new InvalidOperationException("千问响应不是有效 JSON 对象。", ex);
        }
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
                return TextElementTruncator.Truncate(message.GetString() ?? "", 160);
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
