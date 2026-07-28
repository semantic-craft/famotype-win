using System.Text.Encodings.Web;
using System.Text.Json;
using Famo.Settings.Core;

namespace Famo.Settings.Core.Clipboard;

public sealed class ClipboardHistoryEntry
{
    public string Id { get; set; } = string.Empty;
    public string Text { get; set; } = string.Empty;
    public DateTimeOffset CreatedAt { get; set; }
}

/// <summary>剪贴板历史本地 store：默认关闭、按需捕获、去重置顶、限额持久化。</summary>
public sealed class ClipboardHistoryStore
{
    public const int MaxEntries = 20;
    public const int MaxTextLength = 2000;

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    public string FilePath { get; }

    public ClipboardHistoryStore(string? filePath = null)
    {
        FilePath = filePath ?? FamoPaths.ClipboardHistoryFile;
    }

    /// <summary>供纯展示用：读取/反序列化失败时返回空列表，不抛出。</summary>
    public IReadOnlyList<ClipboardHistoryEntry> Load()
    {
        try
        {
            return LoadOrThrow();
        }
        catch
        {
            return Array.Empty<ClipboardHistoryEntry>();
        }
    }

    /// <summary>与 <see cref="Load"/> 相同，但读取/反序列化失败时直接抛出，不当作空历史处理，
    /// 避免 AddText 的读-改-写用空列表覆盖磁盘上已有的剪贴板历史。</summary>
    private IReadOnlyList<ClipboardHistoryEntry> LoadOrThrow()
    {
        if (!File.Exists(FilePath)) return Array.Empty<ClipboardHistoryEntry>();

        return SafeJsonFile.Read(FilePath, json =>
        {
            List<ClipboardHistoryEntry>? entries =
                JsonSerializer.Deserialize<List<ClipboardHistoryEntry>>(json, JsonOptions);
            return (IReadOnlyList<ClipboardHistoryEntry>)(entries ?? new List<ClipboardHistoryEntry>())
                .Where(e => !string.IsNullOrEmpty(e.Text))
                .Take(MaxEntries)
                .ToArray();
        });
    }

    public bool AddText(string? text, bool enabled, DateTimeOffset? now = null)
    {
        if (!enabled || string.IsNullOrEmpty(text)) return false;

        string captured = TextElementTruncator.Truncate(text, MaxTextLength);
        var entries = LoadOrThrow().Where(e => e.Text != captured).ToList();
        entries.Insert(0, new ClipboardHistoryEntry
        {
            Id = Guid.NewGuid().ToString("N"),
            Text = captured,
            CreatedAt = now ?? DateTimeOffset.Now,
        });
        Save(entries.Take(MaxEntries));
        return true;
    }

    public void Clear()
    {
        if (File.Exists(FilePath))
        {
            File.Delete(FilePath);
        }
    }

    private void Save(IEnumerable<ClipboardHistoryEntry> entries)
    {
        string dir = Path.GetDirectoryName(FilePath)!;
        Directory.CreateDirectory(dir);

        SafeJsonFile.WriteAtomic(FilePath, JsonSerializer.Serialize(entries.ToArray(), JsonOptions));
    }
}
