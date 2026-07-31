namespace Famo.Settings.Core.Ai;

public static class DeepSeekResponsesApi
{
    public const string Endpoint = "https://api.deepseek.com/responses";
    public const string FlashModel = "deepseek-v4-flash";

    private const string Host = "api.deepseek.com";
    private const string ResponsesPath = "/responses";

    internal static Uri ResolveEndpoint(Uri configuredEndpoint, string model)
    {
        if (!model.Equals(FlashModel, StringComparison.OrdinalIgnoreCase)
            || !IsOfficialChatEndpoint(configuredEndpoint))
        {
            return configuredEndpoint;
        }

        return new Uri(Endpoint);
    }

    internal static bool IsResponsesEndpoint(Uri endpoint) =>
        IsExactOfficialEndpoint(endpoint, ResponsesPath);

    private static bool IsOfficialChatEndpoint(Uri endpoint) =>
        IsExactOfficialEndpoint(endpoint, "/chat/completions")
        || IsExactOfficialEndpoint(endpoint, "/v1/chat/completions");

    private static bool IsExactOfficialEndpoint(Uri endpoint, string path) =>
        endpoint.Scheme == Uri.UriSchemeHttps
        && endpoint.IsDefaultPort
        && endpoint.UserInfo.Length == 0
        && endpoint.Query.Length == 0
        && endpoint.Fragment.Length == 0
        && endpoint.Host.Equals(Host, StringComparison.OrdinalIgnoreCase)
        && endpoint.AbsolutePath.Equals(path, StringComparison.Ordinal);
}
