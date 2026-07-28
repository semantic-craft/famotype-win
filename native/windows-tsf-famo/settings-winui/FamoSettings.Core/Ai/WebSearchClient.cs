using System.Globalization;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;

namespace Famo.Settings.Core.Ai;

public static class WebSearchBackends
{
    public const string Doubao = "doubao";
    public const string Perplexity = "perplexity";

    public static readonly string[] All = [Doubao, Perplexity];

    public static string Normalize(string? backend) =>
        backend == Perplexity ? Perplexity : Doubao;

    public static string DisplayName(string backend) =>
        Normalize(backend) == Perplexity ? "Perplexity" : "豆包搜索";

    public static string SecretName(string backend) =>
        $"famo.websearch.{Normalize(backend)}.apiKey";

    public static string Endpoint(string backend) =>
        Normalize(backend) == Perplexity
            ? "https://api.perplexity.ai/search"
            : "https://open.feedcoopapi.com/search_api/global_search";

    public static string KeyHint(string backend) =>
        Normalize(backend) == Perplexity
            ? "在 perplexity.ai 的 API 设置中创建（pplx- 开头）；国内网络可能需要代理。"
            : "在火山引擎「联网搜索控制台 → API Key 管理 → 按量后付费」单独创建；方舟模型 Key 在这里无效。";

    internal static IEnumerable<string> Ordered(string preferred)
    {
        string first = Normalize(preferred);
        yield return first;
        yield return first == Doubao ? Perplexity : Doubao;
    }
}

internal sealed record WebSearchResult(
    string Title,
    string Url,
    string Snippet,
    string? PublishedAt,
    string? Hostname);

internal sealed record WebSearchGrounding(string Provider, string Context);

/// <summary>任意提问的独立搜索层：先取网页结果，再交给当前默认模型作答。</summary>
internal sealed class WebSearchClient
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    private readonly ISecretStore _secrets;
    private readonly HttpClient _http;

    public WebSearchClient(ISecretStore secrets, HttpClient? http = null)
    {
        _secrets = secrets;
        _http = http ?? new HttpClient();
    }

    public async Task<WebSearchGrounding?> SearchAsync(
        string preferredBackend,
        string query,
        CancellationToken cancellationToken)
    {
        if (string.IsNullOrWhiteSpace(query)) return null;

        foreach (string backend in WebSearchBackends.Ordered(preferredBackend))
        {
            string? apiKey;
            try
            {
                apiKey = _secrets.GetSecret(WebSearchBackends.SecretName(backend))?.Trim();
            }
            catch
            {
                continue;
            }
            if (string.IsNullOrEmpty(apiKey)) continue;

            try
            {
                IReadOnlyList<WebSearchResult> results = await SearchBackendAsync(
                    backend, query, apiKey, cancellationToken);
                if (results.Count == 0) continue;
                return new WebSearchGrounding(
                    WebSearchBackends.DisplayName(backend),
                    BuildContext(results));
            }
            catch (WebSearchRouteUnavailableException)
            {
                // Key/套餐/参数配置拒绝时尝试另一家；全部不可用则退回普通问答。
            }
        }
        return null;
    }

    private async Task<IReadOnlyList<WebSearchResult>> SearchBackendAsync(
        string backend,
        string query,
        string apiKey,
        CancellationToken cancellationToken)
    {
        bool perplexity = WebSearchBackends.Normalize(backend) == WebSearchBackends.Perplexity;
        object body = perplexity
            ? new
            {
                query = query.Trim(),
                max_results = 5,
                search_context_size = "high",
            }
            : new
            {
                Query = TruncateTextElements(query.Trim(), 100),
                DocCount = 5,
                MaxSnippetLength = 800,
                MaxImageCountPerDoc = 1,
            };

        using var request = new HttpRequestMessage(HttpMethod.Post, WebSearchBackends.Endpoint(backend));
        request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", apiKey);
        request.Content = new StringContent(
            JsonSerializer.Serialize(body, JsonOptions), Encoding.UTF8, "application/json");

        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(TimeSpan.FromSeconds(8));
        using HttpResponseMessage response = await _http.SendAsync(
            request, HttpCompletionOption.ResponseHeadersRead, timeout.Token);
        byte[] payload;
        try
        {
            payload = await BoundedHttpContent.ReadBytesAsync(
                response.Content, timeout.Token);
        }
        catch (InvalidDataException)
        {
            throw new WebSearchRouteUnavailableException();
        }
        if (!response.IsSuccessStatusCode)
        {
            int status = (int)response.StatusCode;
            if (status is >= 400 and <= 499 && status is not 408 and not 429)
                throw new WebSearchRouteUnavailableException();
            throw new InvalidOperationException($"联网搜索失败：HTTP {status}");
        }

        try
        {
            using JsonDocument document = JsonDocument.Parse(Sanitize(payload));
            return perplexity
                ? ParsePerplexity(document.RootElement)
                : ParseDoubao(document.RootElement);
        }
        catch (JsonException ex)
        {
            throw new InvalidOperationException("联网搜索响应格式无法解析。", ex);
        }
    }

    private static IReadOnlyList<WebSearchResult> ParseDoubao(JsonElement root)
    {
        if (root.TryGetProperty("ResponseMetadata", out JsonElement metadata)
            && metadata.TryGetProperty("Error", out JsonElement error)
            && error.ValueKind == JsonValueKind.Object)
        {
            ThrowServiceError(
                Scalar(error, "Code", "CodeN"),
                Scalar(error, "Message"));
        }
        if (!root.TryGetProperty("Result", out JsonElement result))
            throw new JsonException("Missing Result");
        if (result.TryGetProperty("ErrorCode", out JsonElement errorCode)
            && Scalar(errorCode) is string code && code != "0")
        {
            ThrowServiceError(code, Scalar(result, "ErrorMsg"));
        }
        if (!result.TryGetProperty("Documents", out JsonElement documents)
            || documents.ValueKind != JsonValueKind.Array)
            return [];

        var parsed = new List<WebSearchResult>();
        foreach (JsonElement document in documents.EnumerateArray())
        {
            string? url = Scalar(document, "Url");
            string? title = Scalar(document, "Title");
            if (string.IsNullOrWhiteSpace(url) || string.IsNullOrWhiteSpace(title)) continue;

            var snippets = new List<string>();
            if (document.TryGetProperty("Snippet", out JsonElement snippet)
                && snippet.ValueKind == JsonValueKind.Array)
            {
                foreach (JsonElement part in snippet.EnumerateArray())
                {
                    if (Scalar(part, "Type") == "text"
                        && Scalar(part, "Text") is string text
                        && !string.IsNullOrWhiteSpace(text))
                        snippets.Add(text.Trim());
                }
            }
            parsed.Add(new WebSearchResult(
                title,
                url,
                string.Join("\n", snippets),
                NestedScalar(document, "DocumentInfo", "PublishTime"),
                NestedScalar(document, "HostInfo", "Hostname")));
        }
        return parsed;
    }

    private static IReadOnlyList<WebSearchResult> ParsePerplexity(JsonElement root)
    {
        if (root.TryGetProperty("error", out JsonElement error)
            && error.ValueKind == JsonValueKind.Object)
            ThrowServiceError(Scalar(error, "type"), Scalar(error, "message"));
        if (!root.TryGetProperty("results", out JsonElement results)
            || results.ValueKind != JsonValueKind.Array)
            throw new JsonException("Missing results");

        var parsed = new List<WebSearchResult>();
        foreach (JsonElement result in results.EnumerateArray())
        {
            string? url = Scalar(result, "url");
            string? title = Scalar(result, "title");
            if (string.IsNullOrWhiteSpace(url) || string.IsNullOrWhiteSpace(title)) continue;
            string? published = Scalar(result, "date");
            if (string.IsNullOrWhiteSpace(published)) published = Scalar(result, "last_updated");
            string? host = null;
            if (Uri.TryCreate(url, UriKind.Absolute, out Uri? uri))
                host = uri.Host.StartsWith("www.", StringComparison.OrdinalIgnoreCase)
                    ? uri.Host[4..]
                    : uri.Host;
            parsed.Add(new WebSearchResult(
                title,
                url,
                Scalar(result, "snippet")?.Trim() ?? "",
                published,
                host));
        }
        return parsed;
    }

    private static void ThrowServiceError(string? code, string? message)
    {
        if (code is "700429" or "10500" or "10501" or "rate_limit_error")
            throw new InvalidOperationException($"联网搜索失败：{code} {message}".Trim());
        throw new WebSearchRouteUnavailableException();
    }

    private static string BuildContext(IReadOnlyList<WebSearchResult> results)
    {
        var entries = new List<string>(results.Count);
        for (int index = 0; index < results.Count; index++)
        {
            WebSearchResult result = results[index];
            var lines = new List<string> { $"[{index + 1}] {result.Title}" };
            if (!string.IsNullOrWhiteSpace(result.Hostname)) lines.Add($"来源：{result.Hostname}");
            if (!string.IsNullOrWhiteSpace(result.PublishedAt)) lines.Add($"发布时间：{result.PublishedAt}");
            lines.Add($"网址：{result.Url}");
            if (!string.IsNullOrWhiteSpace(result.Snippet)) lines.Add($"摘要：{result.Snippet}");
            entries.Add(string.Join("\n", lines));
        }
        return "以下是刚刚检索到的网页结果，时效性高于你的训练知识。回答时以它们为准；" +
            "引用具体事实时用 [序号] 标注来源。如果这些结果答不上问题，就直说没有查到，不要编造。\n\n" +
            string.Join("\n\n", entries);
    }

    private static byte[] Sanitize(byte[] data)
    {
        byte[] sanitized = data.ToArray();
        for (int index = 0; index < sanitized.Length; index++)
        {
            if (sanitized[index] < 0x20) sanitized[index] = 0x20;
        }
        return sanitized;
    }

    private static string TruncateTextElements(string value, int max)
    {
        var info = new StringInfo(value);
        return info.SubstringByTextElements(0, Math.Min(info.LengthInTextElements, max));
    }

    private static string? NestedScalar(JsonElement element, string parent, string child) =>
        element.TryGetProperty(parent, out JsonElement nested) ? Scalar(nested, child) : null;

    private static string? Scalar(JsonElement element, params string[] names)
    {
        foreach (string name in names)
        {
            if (element.ValueKind == JsonValueKind.Object
                && element.TryGetProperty(name, out JsonElement value))
                return Scalar(value);
        }
        return null;
    }

    private static string? Scalar(JsonElement value) => value.ValueKind switch
    {
        JsonValueKind.String => value.GetString(),
        JsonValueKind.Number => value.GetRawText(),
        _ => null,
    };

    private sealed class WebSearchRouteUnavailableException : Exception;
}
