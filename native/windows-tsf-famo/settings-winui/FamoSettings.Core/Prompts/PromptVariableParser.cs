using System.Text.RegularExpressions;

namespace Famo.Settings.Core.Prompts;

public sealed record PromptVariable(string Name, string? DefaultValue, string Placeholder);

public static class PromptVariableParser
{
    private static readonly Regex VariableRegex =
        new(@"\{\{([^{}]+)\}\}", RegexOptions.Compiled);

    public static IReadOnlyList<PromptVariable> Extract(string? content)
    {
        if (string.IsNullOrEmpty(content)) return Array.Empty<PromptVariable>();

        var variables = new List<PromptVariable>();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (Match match in VariableRegex.Matches(content))
        {
            PromptVariable? variable = Parse(match);
            if (variable is null || !seen.Add(variable.Name)) continue;
            variables.Add(variable);
        }

        return variables;
    }

    internal static string Replace(string content, Func<PromptVariable, string?> replacement)
    {
        if (string.IsNullOrEmpty(content)) return string.Empty;

        return VariableRegex.Replace(content, match =>
        {
            PromptVariable? variable = Parse(match);
            if (variable is null) return match.Value;
            string? value = replacement(variable);
            return value is null ? match.Value : value;
        });
    }

    private static PromptVariable? Parse(Match match)
    {
        string raw = match.Groups[1].Value.Trim();
        if (raw.Length == 0) return null;

        string name = raw;
        string? defaultValue = null;
        int colon = raw.IndexOf(':');
        if (colon >= 0)
        {
            name = raw[..colon].Trim();
            defaultValue = raw[(colon + 1)..].Trim();
        }

        return name.Length == 0
            ? null
            : new PromptVariable(name, defaultValue, match.Value);
    }
}

public static class PromptRenderer
{
    public static string Render(string? content, IReadOnlyDictionary<string, string> values)
    {
        if (string.IsNullOrEmpty(content)) return string.Empty;
        return PromptVariableParser.Replace(content, variable =>
            values.TryGetValue(variable.Name, out string? value) ? value : null);
    }
}
