namespace Famo.Settings.Core;

public sealed record SettingsPageDef(string Id, string Badge, string Title, string Glyph);

public static class SettingsNavigation
{
    public static readonly IReadOnlyList<SettingsPageDef> VisiblePages =
    [
        new("keyboard", "键", "键盘输入", ""), // KeyboardClassic
        new("shortcuts", "捷", "快捷键设置", ""), // KeyboardShortcut
        new("candidate", "候", "候选窗设置", ""), // Color
        new("quick-phrases", "短", "快捷短语", ""), // QuickNote
        new("clipboard", "贴", "剪贴板", ""), // ClipboardList
        new("skills", "技", "技能平台", ""), // Dictionary
        new("ai", "智", "AI 助手", ""), // Robot
        new("status-bar", "浮", "悬浮状态栏", ""), // Pinned (常驻可拖动悬浮条)
        new("skin", "皮", "皮肤外观", ""), // Personalize
        new("about", "关", "关于", ""), // Info
    ];

    private static readonly IReadOnlyDictionary<string, string> HiddenPageParents = new Dictionary<string, string>
    {
        ["prompt-library"] = "ai",
    };

    private static readonly IReadOnlyDictionary<string, string> Aliases = new Dictionary<string, string>
    {
        ["input"] = "keyboard",
        ["convenience"] = "keyboard",
        ["switches"] = "keyboard",
        ["deploy"] = "about",
    };

    public static string ResolvePageId(string? startPage)
    {
        if (string.IsNullOrWhiteSpace(startPage))
        {
            return "keyboard";
        }

        string normalized = startPage.Trim().ToLowerInvariant();
        if (Aliases.TryGetValue(normalized, out string? aliasTarget))
        {
            return aliasTarget;
        }

        if (VisiblePages.Any(page => page.Id == normalized) || HiddenPageParents.ContainsKey(normalized))
        {
            return normalized;
        }

        return "keyboard";
    }

    public static string VisibleParentPageId(string pageId)
    {
        string resolved = ResolvePageId(pageId);
        return HiddenPageParents.TryGetValue(resolved, out string? parent) ? parent : resolved;
    }
}
