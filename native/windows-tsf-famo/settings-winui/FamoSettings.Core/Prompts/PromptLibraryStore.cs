using System.Text.Encodings.Web;
using System.Text.Json;
using Famo.Settings.Core;

namespace Famo.Settings.Core.Prompts;

public sealed class PromptLibraryDocument
{
    public int Version { get; set; } = 1;
    public List<PromptCategory> Categories { get; set; } = new();
    public List<PromptLibraryEntry> Prompts { get; set; } = new();
}

public sealed class PromptCategory
{
    public string Id { get; set; } = string.Empty;
    public string Name { get; set; } = string.Empty;
    public string? Color { get; set; }
    public bool Enabled { get; set; } = true;
    public DateTimeOffset CreatedAt { get; set; }
    public DateTimeOffset UpdatedAt { get; set; }
}

public sealed class PromptLibraryEntry
{
    public string Id { get; set; } = string.Empty;
    public string Title { get; set; } = string.Empty;
    public string Content { get; set; } = string.Empty;
    public string CategoryId { get; set; } = PromptLibraryStore.DefaultCategoryId;
    public List<string> Tags { get; set; } = new();
    public bool Enabled { get; set; } = true;
    public bool Pinned { get; set; }
    public string? Trigger { get; set; }
    public string? Notes { get; set; }
    public string? SourceUrl { get; set; }
    public DateTimeOffset CreatedAt { get; set; }
    public DateTimeOffset UpdatedAt { get; set; }
}

public sealed record PromptLibraryImportResult(
    int ImportedPrompts,
    int SkippedPrompts,
    int ImportedCategories,
    int SkippedCategories);

public sealed class PromptLibraryStore
{
    public const string DefaultCategoryId = "default";
    public const int Version = 1;
    public const int MaxGeneratedTitleLength = 80;

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = true,
        WriteIndented = true,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    public string FilePath { get; }

    public PromptLibraryStore(string? filePath = null)
    {
        FilePath = filePath ?? FamoPaths.PromptLibraryFile;
    }

    /// <summary>供纯展示用：读取/反序列化失败时（先备份为 .bak）回退空文档，不抛出。</summary>
    public PromptLibraryDocument Load()
    {
        try
        {
            return LoadOrThrow();
        }
        catch
        {
            return CreateDefaultDocument();
        }
    }

    /// <summary>与 <see cref="Load"/> 相同，但读取/反序列化失败时直接抛出，不当作空文档处理，
    /// 避免 UpsertPrompt/DeletePrompt/UpsertCategory/DeleteCategory/Import 的读-改-写
    /// 用空文档覆盖磁盘上已有的提示词库。</summary>
    private PromptLibraryDocument LoadOrThrow()
    {
        if (!File.Exists(FilePath)) return CreateDefaultDocument();

        return SafeJsonFile.Read(FilePath, json =>
        {
            PromptLibraryDocument? document =
                JsonSerializer.Deserialize<PromptLibraryDocument>(json, JsonOptions);
            return NormalizeDocument(document ?? CreateDefaultDocument(), skipInvalidPrompts: true);
        });
    }

    public void Save(PromptLibraryDocument document)
    {
        WriteDocument(NormalizeDocument(document, skipInvalidPrompts: false));
    }

    public PromptLibraryEntry UpsertPrompt(PromptLibraryEntry entry, DateTimeOffset? now = null)
    {
        DateTimeOffset timestamp = now ?? DateTimeOffset.Now;
        PromptLibraryEntry normalized = NormalizePrompt(entry, timestamp);
        var document = LoadOrThrow();
        int index = document.Prompts.FindIndex(p => string.Equals(p.Id, normalized.Id, StringComparison.Ordinal));
        if (index >= 0)
        {
            normalized.CreatedAt = document.Prompts[index].CreatedAt == default
                ? timestamp
                : document.Prompts[index].CreatedAt;
            document.Prompts[index] = normalized;
        }
        else
        {
            document.Prompts.Add(normalized);
        }

        WriteDocument(NormalizeDocument(document, skipInvalidPrompts: false));
        return normalized;
    }

    public bool DeletePrompt(string id)
    {
        string normalizedId = (id ?? string.Empty).Trim();
        if (string.IsNullOrEmpty(normalizedId)) return false;

        var document = LoadOrThrow();
        int removed = document.Prompts.RemoveAll(p => string.Equals(p.Id, normalizedId, StringComparison.Ordinal));
        if (removed == 0) return false;

        WriteDocument(document);
        return true;
    }

    public PromptCategory UpsertCategory(PromptCategory category, DateTimeOffset? now = null)
    {
        DateTimeOffset timestamp = now ?? DateTimeOffset.Now;
        PromptCategory normalized = NormalizeCategory(category, timestamp);
        var document = LoadOrThrow();
        int index = document.Categories.FindIndex(c => string.Equals(c.Id, normalized.Id, StringComparison.Ordinal));
        if (index >= 0)
        {
            normalized.CreatedAt = document.Categories[index].CreatedAt == default
                ? timestamp
                : document.Categories[index].CreatedAt;
            document.Categories[index] = normalized;
        }
        else
        {
            document.Categories.Add(normalized);
        }

        WriteDocument(NormalizeDocument(document, skipInvalidPrompts: false));
        return normalized;
    }

    public bool DeleteCategory(string id)
    {
        string normalizedId = (id ?? string.Empty).Trim();
        if (string.IsNullOrEmpty(normalizedId) || normalizedId == DefaultCategoryId) return false;

        var document = LoadOrThrow();
        int removed = document.Categories.RemoveAll(c => string.Equals(c.Id, normalizedId, StringComparison.Ordinal));
        if (removed == 0) return false;

        foreach (PromptLibraryEntry prompt in document.Prompts.Where(p => p.CategoryId == normalizedId))
        {
            prompt.CategoryId = DefaultCategoryId;
        }

        WriteDocument(NormalizeDocument(document, skipInvalidPrompts: false));
        return true;
    }

    public PromptLibraryImportResult Import(PromptLibraryDocument incoming, DateTimeOffset? now = null)
    {
        DateTimeOffset timestamp = now ?? DateTimeOffset.Now;
        var document = LoadOrThrow();
        int importedCategories = 0;
        int skippedCategories = 0;
        int importedPrompts = 0;
        int skippedPrompts = 0;

        foreach (PromptCategory category in incoming.Categories ?? new List<PromptCategory>())
        {
            try
            {
                PromptCategory normalized = NormalizeCategory(category, timestamp);
                int index = document.Categories.FindIndex(c => c.Id == normalized.Id);
                if (index >= 0)
                {
                    normalized.CreatedAt = document.Categories[index].CreatedAt == default
                        ? timestamp
                        : document.Categories[index].CreatedAt;
                    document.Categories[index] = normalized;
                }
                else
                {
                    document.Categories.Add(normalized);
                }
                importedCategories++;
            }
            catch (InvalidDataException)
            {
                skippedCategories++;
            }
        }

        foreach (PromptLibraryEntry prompt in incoming.Prompts ?? new List<PromptLibraryEntry>())
        {
            try
            {
                PromptLibraryEntry normalized = NormalizePrompt(prompt, timestamp);
                EnsureCategory(document, normalized.CategoryId, timestamp);
                int index = document.Prompts.FindIndex(p => p.Id == normalized.Id);
                if (index >= 0)
                {
                    normalized.CreatedAt = document.Prompts[index].CreatedAt == default
                        ? timestamp
                        : document.Prompts[index].CreatedAt;
                    document.Prompts[index] = normalized;
                }
                else
                {
                    document.Prompts.Add(normalized);
                }
                importedPrompts++;
            }
            catch (InvalidDataException)
            {
                skippedPrompts++;
            }
        }

        WriteDocument(NormalizeDocument(document, skipInvalidPrompts: false));
        return new PromptLibraryImportResult(
            importedPrompts,
            skippedPrompts,
            importedCategories,
            skippedCategories);
    }

    public PromptLibraryImportResult ImportJson(string json, DateTimeOffset? now = null)
    {
        PromptLibraryDocument document = JsonSerializer.Deserialize<PromptLibraryDocument>(json, JsonOptions)
            ?? throw new InvalidDataException("提示词库 JSON 解析失败");
        return Import(document, now);
    }

    public string ExportJson() =>
        JsonSerializer.Serialize(Load(), JsonOptions);

    public static PromptLibraryDocument CreateDefaultDocument(DateTimeOffset? now = null)
    {
        DateTimeOffset timestamp = now ?? DateTimeOffset.Now;
        return new PromptLibraryDocument
        {
            Version = Version,
            Categories =
            [
                new PromptCategory
                {
                    Id = DefaultCategoryId,
                    Name = "默认",
                    Enabled = true,
                    CreatedAt = timestamp,
                    UpdatedAt = timestamp,
                },
            ],
        };
    }

    public static IReadOnlyList<PromptLibraryEntry> FilterAndSort(
        IEnumerable<PromptLibraryEntry> prompts,
        string? searchTerm = null,
        string? categoryId = null,
        bool includeDisabled = false)
    {
        string query = (searchTerm ?? string.Empty).Trim();
        string category = (categoryId ?? string.Empty).Trim();

        IEnumerable<PromptLibraryEntry> filtered =
            includeDisabled ? prompts : prompts.Where(p => p.Enabled);
        if (!string.IsNullOrEmpty(category))
        {
            filtered = filtered.Where(p => string.Equals(p.CategoryId, category, StringComparison.Ordinal));
        }

        if (!string.IsNullOrEmpty(query))
        {
            filtered = filtered.Where(p => Matches(p, query));
        }

        return filtered
            .OrderByDescending(p => p.Pinned)
            .ThenByDescending(p => p.UpdatedAt)
            .ThenBy(p => p.Title, StringComparer.CurrentCulture)
            .ToArray();
    }

    private void WriteDocument(PromptLibraryDocument document)
    {
        string dir = Path.GetDirectoryName(FilePath)!;
        Directory.CreateDirectory(dir);
        WriteAtomic(FilePath, JsonSerializer.Serialize(document, JsonOptions));
    }

    private static PromptLibraryDocument NormalizeDocument(PromptLibraryDocument document, bool skipInvalidPrompts)
    {
        var normalized = new PromptLibraryDocument { Version = Version };
        DateTimeOffset timestamp = DateTimeOffset.Now;

        foreach (PromptCategory category in document.Categories ?? new List<PromptCategory>())
        {
            try
            {
                PromptCategory clean = NormalizeCategory(category, timestamp);
                int index = normalized.Categories.FindIndex(c => string.Equals(c.Id, clean.Id, StringComparison.Ordinal));
                if (index >= 0)
                    normalized.Categories[index] = clean;
                else
                    normalized.Categories.Add(clean);
            }
            catch (InvalidDataException)
            {
            }
        }

        EnsureCategory(normalized, DefaultCategoryId, timestamp, "默认");

        foreach (PromptLibraryEntry prompt in document.Prompts ?? new List<PromptLibraryEntry>())
        {
            try
            {
                PromptLibraryEntry clean = NormalizePrompt(prompt, timestamp);
                EnsureCategory(normalized, clean.CategoryId, timestamp);
                int index = normalized.Prompts.FindIndex(p => string.Equals(p.Id, clean.Id, StringComparison.Ordinal));
                if (index >= 0)
                    normalized.Prompts[index] = clean;
                else
                    normalized.Prompts.Add(clean);
            }
            catch (InvalidDataException) when (skipInvalidPrompts)
            {
            }
        }

        normalized.Categories = normalized.Categories
            .OrderBy(c => c.Id == DefaultCategoryId ? 0 : 1)
            .ThenBy(c => c.Name, StringComparer.CurrentCulture)
            .ToList();
        normalized.Prompts = SortForStorage(normalized.Prompts).ToList();
        return normalized;
    }

    private static PromptCategory NormalizeCategory(PromptCategory category, DateTimeOffset now)
    {
        string id = NormalizeId(category.Id);
        string name = (category.Name ?? string.Empty).Trim();
        if (string.IsNullOrEmpty(id)) throw new InvalidDataException("分类 ID 不能为空");
        if (string.IsNullOrEmpty(name)) name = id == DefaultCategoryId ? "默认" : id;

        DateTimeOffset created = category.CreatedAt == default ? now : category.CreatedAt;
        DateTimeOffset updated = category.UpdatedAt == default ? created : category.UpdatedAt;
        return new PromptCategory
        {
            Id = id,
            Name = name,
            Color = string.IsNullOrWhiteSpace(category.Color) ? null : category.Color.Trim(),
            Enabled = category.Enabled,
            CreatedAt = created,
            UpdatedAt = updated,
        };
    }

    private static PromptLibraryEntry NormalizePrompt(PromptLibraryEntry prompt, DateTimeOffset now)
    {
        string id = NormalizeId(prompt.Id);
        if (string.IsNullOrEmpty(id)) id = Guid.NewGuid().ToString("N");
        if (string.IsNullOrWhiteSpace(prompt.Content)) throw new InvalidDataException("提示词内容不能为空");

        string categoryId = NormalizeId(prompt.CategoryId);
        if (string.IsNullOrEmpty(categoryId)) categoryId = DefaultCategoryId;
        DateTimeOffset created = prompt.CreatedAt == default ? now : prompt.CreatedAt;
        DateTimeOffset updated = prompt.UpdatedAt == default ? now : prompt.UpdatedAt;

        return new PromptLibraryEntry
        {
            Id = id,
            Title = NormalizeTitle(prompt.Title, prompt.Content),
            Content = prompt.Content,
            CategoryId = categoryId,
            Tags = NormalizeTags(prompt.Tags),
            Enabled = prompt.Enabled,
            Pinned = prompt.Pinned,
            Trigger = NullIfWhiteSpace(prompt.Trigger),
            Notes = NullIfWhiteSpace(prompt.Notes),
            SourceUrl = NullIfWhiteSpace(prompt.SourceUrl),
            CreatedAt = created,
            UpdatedAt = updated,
        };
    }

    private static void EnsureCategory(
        PromptLibraryDocument document,
        string id,
        DateTimeOffset now,
        string? name = null)
    {
        if (document.Categories.Any(c => string.Equals(c.Id, id, StringComparison.Ordinal))) return;

        document.Categories.Add(new PromptCategory
        {
            Id = id,
            Name = string.IsNullOrWhiteSpace(name) ? id : name,
            Enabled = true,
            CreatedAt = now,
            UpdatedAt = now,
        });
    }

    private static string NormalizeId(string? id) =>
        (id ?? string.Empty).Trim();

    private static string NormalizeTitle(string? title, string content)
    {
        string clean = (title ?? string.Empty).Trim();
        if (string.IsNullOrEmpty(clean))
        {
            clean = content
                .Split(new[] { "\r\n", "\n", "\r" }, StringSplitOptions.None)
                .Select(line => line.Trim())
                .FirstOrDefault(line => line.Length > 0) ?? "未命名提示词";
        }

        return TextElementTruncator.Truncate(clean, MaxGeneratedTitleLength);
    }

    private static List<string> NormalizeTags(IEnumerable<string>? tags)
    {
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var result = new List<string>();
        foreach (string tag in tags ?? Array.Empty<string>())
        {
            string clean = tag.Trim();
            if (clean.Length == 0 || !seen.Add(clean)) continue;
            result.Add(clean);
        }
        return result;
    }

    private static string? NullIfWhiteSpace(string? value)
    {
        string clean = (value ?? string.Empty).Trim();
        return clean.Length == 0 ? null : clean;
    }

    private static bool Matches(PromptLibraryEntry prompt, string query) =>
        Contains(prompt.Title, query)
        || Contains(prompt.Content, query)
        || Contains(prompt.CategoryId, query)
        || Contains(prompt.Trigger, query)
        || prompt.Tags.Any(tag => Contains(tag, query));

    private static bool Contains(string? value, string query) =>
        (value ?? string.Empty).Contains(query, StringComparison.CurrentCultureIgnoreCase);

    private static IEnumerable<PromptLibraryEntry> SortForStorage(IEnumerable<PromptLibraryEntry> prompts) =>
        prompts
            .OrderByDescending(p => p.Pinned)
            .ThenByDescending(p => p.UpdatedAt)
            .ThenBy(p => p.Title, StringComparer.CurrentCulture);

    private static void WriteAtomic(string path, string content) => SafeJsonFile.WriteAtomic(path, content);
}
