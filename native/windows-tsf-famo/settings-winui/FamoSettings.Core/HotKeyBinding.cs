namespace Famo.Settings.Core;

/// <summary>Windows 功能召唤热键的单一校验/规范化点。</summary>
public static class HotKeyBinding
{
    public static string? Create(char key, bool control, bool alt, bool shift)
    {
        char upper = char.ToUpperInvariant(key);
        if (upper is < 'A' or > 'Z') return null;
        if ((control ? 1 : 0) + (alt ? 1 : 0) + (shift ? 1 : 0) < 2) return null;

        var parts = new List<string>(4);
        if (control) parts.Add("Ctrl");
        if (alt) parts.Add("Alt");
        if (shift) parts.Add("Shift");
        parts.Add(upper.ToString());
        return string.Join('+', parts);
    }

    public static string? Normalize(string? value)
    {
        if (string.IsNullOrWhiteSpace(value)) return string.Empty;
        string[] parts = value.Split('+', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length < 3 || parts[^1].Length != 1) return null;

        bool control = false, alt = false, shift = false;
        foreach (string part in parts[..^1])
        {
            if (part.Equals("Ctrl", StringComparison.OrdinalIgnoreCase)) control = true;
            else if (part.Equals("Alt", StringComparison.OrdinalIgnoreCase)) alt = true;
            else if (part.Equals("Shift", StringComparison.OrdinalIgnoreCase)) shift = true;
            else return null;
        }
        return Create(parts[^1][0], control, alt, shift);
    }
}
