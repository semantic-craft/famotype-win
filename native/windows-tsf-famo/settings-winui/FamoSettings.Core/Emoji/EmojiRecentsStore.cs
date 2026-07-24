using System.Globalization;
using System.Text.Json;

namespace Famo.Settings.Core.Emoji;

/// <summary>
/// 「最近」表情 store，文件后端 <c>%LOCALAPPDATA%\Famo\emoji-recent.json</c>
/// （对位 macOS UserDefaults <c>FamoEmojiRecent</c>，ADR-0004：只落 %LOCALAPPDATA%\Famo）。
/// 首次返回起始集；<see cref="Push"/> 去重 + 前插 + 上限 14。路径可注入供测试。
/// </summary>
public sealed class EmojiRecentsStore
{
    private const int Cap = 14;

    private static readonly string[] Seed =
        { "😂", "❤️", "👍", "《》", "「」", "§", "—", "→", "🎉", "✓", "°", "…" };

    /// <summary>recent 列表落点（默认 %LOCALAPPDATA%\Famo\emoji-recent.json）。</summary>
    public string FilePath { get; }

    public EmojiRecentsStore(string? filePath = null)
    {
        FilePath = filePath ?? Path.Combine(FamoPaths.FamoDir, "emoji-recent.json");
    }

    /// <summary>当前「最近」字符列表；文件缺失或损坏时回退起始集。</summary>
    public IReadOnlyList<string> Current()
    {
        if (!File.Exists(FilePath)) return Seed;
        try
        {
            return SafeJsonFile.Read(FilePath, json =>
            {
                string[]? list = JsonSerializer.Deserialize<string[]>(json);
                return list is { Length: > 0 } ? list : Seed;
            });
        }
        catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException)
        {
            return Seed;
        }
    }

    /// <summary>把一次选择推到最前：去重 + 前插 + 上限 14，落盘后返回新列表。</summary>
    public IReadOnlyList<string> Push(string ch)
    {
        var list = new List<string>(Current().Count + 1);
        list.Add(ch);
        foreach (string c in Current())
            if (c != ch) list.Add(c);
        if (list.Count > Cap) list.RemoveRange(Cap, list.Count - Cap);

        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            SafeJsonFile.WriteAtomic(FilePath, JsonSerializer.Serialize(list));
        }
        catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException)
        {
            // 写盘失败时跳过持久化，仍返回内存里算好的新列表
        }
        return list;
    }

    /// <summary>「最近」作为字形（wide 由字素簇数推断，对齐 macOS <c>$0.count &gt; 3</c>）。</summary>
    public IReadOnlyList<FamoGlyph> Glyphs()
    {
        var result = new List<FamoGlyph>();
        foreach (string c in Current())
            result.Add(new FamoGlyph(c, "", GraphemeCount(c) > 3));
        return result;
    }

    /// <summary>字素簇（text element）计数，等价 Swift String.count。</summary>
    private static int GraphemeCount(string s)
    {
        int n = 0;
        var e = StringInfo.GetTextElementEnumerator(s);
        while (e.MoveNext()) n++;
        return n;
    }
}
