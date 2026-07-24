namespace Famo.Settings.Core;

/// <summary>
/// 形态甲：法墨配置只落 %LOCALAPPDATA%\Famo，根本不碰 %AppData%\Rime（ADR-0004）。
/// </summary>
public static class FamoPaths
{
    /// <summary>%LOCALAPPDATA%\Famo</summary>
    public static string FamoDir =>
        Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "Famo");

    /// <summary>%LOCALAPPDATA%\Famo\famo-settings.json —— 设置 store 单一真相源。</summary>
    public static string SettingsFile => Path.Combine(FamoDir, "famo-settings.json");

    /// <summary>%LOCALAPPDATA%\Famo\clipboard-history.json —— 剪贴板历史本地 store。</summary>
    public static string ClipboardHistoryFile => Path.Combine(FamoDir, "clipboard-history.json");

    /// <summary>%LOCALAPPDATA%\Famo\quick-phrases.json —— 快捷短语编辑态 store。</summary>
    public static string QuickPhrasesFile => Path.Combine(FamoDir, "quick-phrases.json");

    /// <summary>%LOCALAPPDATA%\Famo\prompt-library.json —— 提示词库本地 store。</summary>
    public static string PromptLibraryFile => Path.Combine(FamoDir, "prompt-library.json");

    /// <summary>%LOCALAPPDATA%\Famo\famo_quick_send.txt —— Rime tabledb 生成态。</summary>
    public static string QuickSendTableFile => Path.Combine(FamoDir, "famo_quick_send.txt");

    /// <summary>%LOCALAPPDATA%\Famo\ai-providers.json —— AI 供应商资料 store；密钥不写入此文件。</summary>
    public static string AiProviderProfilesFile => Path.Combine(FamoDir, "ai-providers.json");
}
