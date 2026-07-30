namespace Famo.Settings.Core.Ai;

public static class QwenResponsesApi
{
    private const string BeijingHostSuffix = ".cn-beijing.maas.aliyuncs.com";
    private const string ResponsesPath = "/compatible-mode/v1/responses";

    public static string BuildBeijingEndpoint(string workspaceId)
    {
        string value = workspaceId.Trim();
        if (!IsDnsHostLabel(value))
        {
            throw new InvalidDataException(
                "Workspace ID 格式无效；请从阿里云百炼业务空间详情复制完整 ID。");
        }

        return $"https://{value}{BeijingHostSuffix}{ResponsesPath}";
    }

    internal static bool IsResponsesEndpoint(Uri endpoint)
    {
        if (endpoint.Scheme != Uri.UriSchemeHttps
            || !endpoint.IsDefaultPort
            || endpoint.UserInfo.Length > 0
            || endpoint.Query.Length > 0
            || endpoint.Fragment.Length > 0
            || !endpoint.AbsolutePath.Equals(ResponsesPath, StringComparison.Ordinal))
        {
            return false;
        }

        string host = endpoint.IdnHost;
        if (!host.EndsWith(BeijingHostSuffix, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        string workspaceId = host[..^BeijingHostSuffix.Length];
        return IsDnsHostLabel(workspaceId);
    }

    private static bool IsDnsHostLabel(string value)
    {
        if (value.Length is < 1 or > 63
            || !IsAsciiLetterOrDigit(value[0])
            || !IsAsciiLetterOrDigit(value[^1]))
        {
            return false;
        }

        return value.All(character =>
            IsAsciiLetterOrDigit(character) || character == '-');
    }

    private static bool IsAsciiLetterOrDigit(char value) =>
        value is >= 'a' and <= 'z'
        or >= 'A' and <= 'Z'
        or >= '0' and <= '9';
}
