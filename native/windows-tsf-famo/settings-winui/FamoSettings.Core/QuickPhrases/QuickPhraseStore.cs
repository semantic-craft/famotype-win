using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Famo.Settings.Core.QuickPhrases;

public sealed class QuickPhraseEntry
{
    public string Text { get; set; } = string.Empty;
    public string Code { get; set; } = string.Empty;
}

/// <summary>快捷短语编辑态 store + Rime tabledb 生成器。</summary>
public sealed class QuickPhraseStore
{
    private const int Weight = 100000;
    private static readonly Regex CodeRx = new(@"^[a-z][a-z0-9]{0,31}$", RegexOptions.Compiled);

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    public string FilePath { get; }

    public QuickPhraseStore(string? filePath = null)
    {
        FilePath = filePath ?? FamoPaths.QuickPhrasesFile;
    }

    public IReadOnlyList<QuickPhraseEntry> Load()
    {
        if (!File.Exists(FilePath)) return Array.Empty<QuickPhraseEntry>();

        try
        {
            return SafeJsonFile.Read(FilePath, json =>
            {
                List<QuickPhraseEntry>? entries =
                    JsonSerializer.Deserialize<List<QuickPhraseEntry>>(json, JsonOptions);
                return Normalized(entries ?? new List<QuickPhraseEntry>(), skipInvalid: true).ToArray();
            });
        }
        catch
        {
            return Array.Empty<QuickPhraseEntry>();
        }
    }

    public void Upsert(QuickPhraseEntry entry)
    {
        QuickPhraseEntry normalized = Normalize(entry);

        var entries = LoadOrThrow().ToList();
        int index = entries.FindIndex(e => e.Code == normalized.Code);
        if (index >= 0)
            entries[index] = normalized;
        else
            entries.Add(normalized);
        Save(entries);
    }

    public void Delete(string code)
    {
        string normalizedCode = NormalizeCode(code);
        Save(LoadOrThrow().Where(e => e.Code != normalizedCode));
    }

    public bool WriteTableDb(string? tablePath = null)
    {
        string path = tablePath ?? FamoPaths.QuickSendTableFile;
        string content = BuildTableDb(LoadOrThrow());
        if (File.Exists(path) && File.ReadAllText(path) == content) return false;

        string dir = Path.GetDirectoryName(path)!;
        Directory.CreateDirectory(dir);
        WriteAtomic(path, content);
        return true;
    }

    /// <summary>与 <see cref="Load"/> 相同，但读取/反序列化失败时直接抛出，不当作空短语库处理，
    /// 避免 Upsert/Delete/WriteTableDb 用空列表覆盖磁盘上已有的短语。</summary>
    private List<QuickPhraseEntry> LoadOrThrow()
    {
        if (!File.Exists(FilePath)) return new List<QuickPhraseEntry>();

        return SafeJsonFile.Read(FilePath, json =>
        {
            List<QuickPhraseEntry>? entries =
                JsonSerializer.Deserialize<List<QuickPhraseEntry>>(json, JsonOptions);
            return Normalized(entries ?? new List<QuickPhraseEntry>(), skipInvalid: true).ToList();
        });
    }

    public static string? Validate(QuickPhraseEntry entry)
    {
        try
        {
            _ = Normalize(entry);
            return null;
        }
        catch (InvalidDataException ex)
        {
            return ex.Message;
        }
    }

    public static string BuildTableDb(IEnumerable<QuickPhraseEntry> entries)
    {
        var sb = new System.Text.StringBuilder();
        sb.Append("# Rime table\n");
        sb.Append("# coding: utf-8\n");
        sb.Append("#@/db_name\tfamo_quick_send\n");
        sb.Append("#@/db_type\ttabledb\n");
        sb.Append("# Famo quick-send phrases. Managed by 法墨输入法 settings.\n\n");

        foreach (QuickPhraseEntry entry in Normalized(entries, skipInvalid: false))
        {
            sb.Append(entry.Text).Append('\t').Append(entry.Code).Append('\t').Append(Weight).Append('\n');
        }
        return sb.ToString();
    }

    private void Save(IEnumerable<QuickPhraseEntry> entries)
    {
        string dir = Path.GetDirectoryName(FilePath)!;
        Directory.CreateDirectory(dir);
        WriteAtomic(FilePath, JsonSerializer.Serialize(Normalized(entries, skipInvalid: false).ToArray(), JsonOptions));
    }

    private static IEnumerable<QuickPhraseEntry> Normalized(IEnumerable<QuickPhraseEntry> entries, bool skipInvalid)
    {
        var byCode = new Dictionary<string, QuickPhraseEntry>(StringComparer.Ordinal);
        foreach (QuickPhraseEntry entry in entries)
        {
            try
            {
                QuickPhraseEntry normalized = Normalize(entry);
                byCode[normalized.Code] = normalized;
            }
            catch (InvalidDataException) when (skipInvalid)
            {
            }
        }

        return byCode.Values.OrderBy(e => e.Code, StringComparer.Ordinal);
    }

    private static QuickPhraseEntry Normalize(QuickPhraseEntry entry)
    {
        string text = (entry.Text ?? string.Empty).Trim();
        string code = NormalizeCode(entry.Code);
        if (string.IsNullOrEmpty(text)) throw new InvalidDataException("短语不能为空");
        if (text.IndexOfAny(new[] { '\r', '\n', '\t' }) >= 0) throw new InvalidDataException("短语不能包含换行或制表符");
        if (!CodeRx.IsMatch(code)) throw new InvalidDataException("编码需小写字母开头，可含数字，最长 32 位");
        return new QuickPhraseEntry { Code = code, Text = text };
    }

    private static string NormalizeCode(string? code) =>
        (code ?? string.Empty).Trim().ToLowerInvariant();

    private static void WriteAtomic(string path, string content) => SafeJsonFile.WriteAtomic(path, content);
}
