using System.Text.Json;
using Famo.Settings.Core;
using Famo.Settings.Core.Prompts;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class PromptLibraryStoreTests : IDisposable
{
    private readonly string _dir;
    private readonly string _file;

    public PromptLibraryStoreTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "famo-prompts-" + Guid.NewGuid().ToString("N"));
        _file = Path.Combine(_dir, "prompt-library.json");
    }

    public void Dispose()
    {
        if (Directory.Exists(_dir)) Directory.Delete(_dir, recursive: true);
    }

    [Fact]
    public void Load_WhenMissingOrInvalid_ReturnsDefaultLocalDocument()
    {
        var store = new PromptLibraryStore(_file);

        PromptLibraryDocument missing = store.Load();
        Assert.Equal(PromptLibraryStore.Version, missing.Version);
        Assert.Contains(missing.Categories, c => c.Id == PromptLibraryStore.DefaultCategoryId && c.Name == "默认");
        Assert.Empty(missing.Prompts);

        Directory.CreateDirectory(_dir);
        File.WriteAllText(_file, "not json");

        PromptLibraryDocument invalid = store.Load();
        Assert.Contains(invalid.Categories, c => c.Id == PromptLibraryStore.DefaultCategoryId);
        Assert.Empty(invalid.Prompts);
    }

    [Fact]
    public void UpsertPrompt_PreservesMultilineChineseContentAndNormalizesMetadata()
    {
        var store = new PromptLibraryStore(_file);
        DateTimeOffset now = new(2026, 7, 1, 8, 0, 0, TimeSpan.Zero);

        PromptLibraryEntry saved = store.UpsertPrompt(new PromptLibraryEntry
        {
            Title = "  ",
            Content = "  第一行标题  \r\n\n请基于{{案由}}生成检索式。",
            CategoryId = " legal ",
            Tags = [" 法律 ", "写作", "法律"],
            Trigger = " ;;case ",
            Notes = "  private note  ",
            SourceUrl = " https://example.com/prompt ",
            Pinned = true,
        }, now);

        PromptLibraryDocument loaded = new PromptLibraryStore(_file).Load();
        PromptLibraryEntry prompt = Assert.Single(loaded.Prompts);
        Assert.Equal(saved.Id, prompt.Id);
        Assert.Equal("第一行标题", prompt.Title);
        Assert.Equal("  第一行标题  \r\n\n请基于{{案由}}生成检索式。", prompt.Content);
        Assert.Equal("legal", prompt.CategoryId);
        Assert.Equal(["法律", "写作"], prompt.Tags);
        Assert.Equal(";;case", prompt.Trigger);
        Assert.Equal("private note", prompt.Notes);
        Assert.Equal("https://example.com/prompt", prompt.SourceUrl);
        Assert.True(prompt.Pinned);
        Assert.Equal(now, prompt.CreatedAt);
        Assert.Equal(now, prompt.UpdatedAt);
        Assert.Contains(loaded.Categories, c => c.Id == "legal" && c.Name == "legal");

        string json = File.ReadAllText(_file);
        Assert.EndsWith("prompt-library.json", FamoPaths.PromptLibraryFile.Replace("\\", "/"), StringComparison.Ordinal);
        Assert.Contains("第一行标题", json);
        Assert.Contains("案由", json);
    }

    [Fact]
    public void UpsertPrompt_UpdatesExistingPromptAndKeepsCreatedAt()
    {
        var store = new PromptLibraryStore(_file);
        DateTimeOffset created = new(2026, 7, 1, 8, 0, 0, TimeSpan.Zero);
        DateTimeOffset updated = created.AddHours(1);

        PromptLibraryEntry first = store.UpsertPrompt(new PromptLibraryEntry
        {
            Id = "p1",
            Title = "旧标题",
            Content = "旧内容",
        }, created);

        PromptLibraryEntry second = store.UpsertPrompt(new PromptLibraryEntry
        {
            Id = "p1",
            Title = "新标题",
            Content = "新内容",
        }, updated);

        PromptLibraryEntry loaded = Assert.Single(store.Load().Prompts);
        Assert.Equal(first.CreatedAt, second.CreatedAt);
        Assert.Equal(created, loaded.CreatedAt);
        Assert.Equal(updated, loaded.UpdatedAt);
        Assert.Equal("新标题", loaded.Title);
        Assert.Equal("新内容", loaded.Content);
    }

    [Fact]
    public void Save_RejectsEmptyPromptContent()
    {
        var store = new PromptLibraryStore(_file);

        Assert.Throws<InvalidDataException>(() => store.Save(new PromptLibraryDocument
        {
            Prompts = [new PromptLibraryEntry { Id = "bad", Title = "空", Content = "  " }],
        }));
    }

    [Fact]
    public void Load_SkipsInvalidPromptsAndLastCategoryWins()
    {
        var document = new PromptLibraryDocument
        {
            Categories =
            [
                new PromptCategory { Id = "legal", Name = "旧分类" },
                new PromptCategory { Id = "legal", Name = "新分类" },
            ],
            Prompts =
            [
                new PromptLibraryEntry { Id = "ok", Title = "可用", Content = "内容", CategoryId = "legal" },
                new PromptLibraryEntry { Id = "bad", Title = "空", Content = "" },
            ],
        };

        var store = new PromptLibraryStore(_file);
        Directory.CreateDirectory(_dir);
        File.WriteAllText(_file, JsonSerializer.Serialize(document));

        PromptLibraryDocument loaded = store.Load();

        Assert.Contains(loaded.Categories, c => c.Id == "legal" && c.Name == "新分类");
        Assert.Single(loaded.Prompts);
        Assert.Equal("ok", loaded.Prompts[0].Id);
    }

    [Fact]
    public void Load_PreservesDisabledPromptsInStore()
    {
        var store = new PromptLibraryStore(_file);
        store.UpsertPrompt(new PromptLibraryEntry
        {
            Id = "disabled",
            Title = "禁用但仍保存",
            Content = "内容",
            Enabled = false,
        });

        PromptLibraryDocument loaded = store.Load();

        PromptLibraryEntry prompt = Assert.Single(loaded.Prompts);
        Assert.False(prompt.Enabled);
    }

    [Fact]
    public void FilterAndSort_SearchesFieldsAndUsesPinnedThenUpdatedOrder()
    {
        DateTimeOffset now = new(2026, 7, 1, 8, 0, 0, TimeSpan.Zero);
        var prompts = new[]
        {
            new PromptLibraryEntry { Id = "old", Title = "普通", Content = "合同审查", CategoryId = "legal", Enabled = true, UpdatedAt = now },
            new PromptLibraryEntry { Id = "disabled", Title = "禁用", Content = "合同审查", CategoryId = "legal", Enabled = false, UpdatedAt = now.AddHours(3) },
            new PromptLibraryEntry { Id = "new", Title = "新", Content = "合同审查", CategoryId = "legal", Enabled = true, UpdatedAt = now.AddHours(1) },
            new PromptLibraryEntry { Id = "pinned", Title = "置顶", Content = "别的内容", CategoryId = "legal", Tags = ["合同"], Enabled = true, Pinned = true, UpdatedAt = now.AddMinutes(10) },
        };

        IReadOnlyList<PromptLibraryEntry> sorted = PromptLibraryStore.FilterAndSort(prompts, "合同", "legal");

        Assert.Equal(["pinned", "new", "old"], sorted.Select(p => p.Id).ToArray());
        Assert.DoesNotContain(sorted, p => p.Id == "disabled");
    }

    [Fact]
    public void Import_MergesByIdGeneratesIdsCreatesMissingCategoriesAndReportsSkips()
    {
        var store = new PromptLibraryStore(_file);
        DateTimeOffset now = new(2026, 7, 1, 8, 0, 0, TimeSpan.Zero);
        store.UpsertPrompt(new PromptLibraryEntry { Id = "p1", Title = "旧", Content = "旧内容" }, now);

        PromptLibraryImportResult result = store.Import(new PromptLibraryDocument
        {
            Categories =
            [
                new PromptCategory { Id = "legal", Name = "法律" },
                new PromptCategory { Id = "", Name = "坏分类" },
            ],
            Prompts =
            [
                new PromptLibraryEntry { Id = "p1", Title = "更新", Content = "新内容", CategoryId = "legal" },
                new PromptLibraryEntry { Title = "新增", Content = "新增内容", CategoryId = "missing" },
                new PromptLibraryEntry { Id = "bad", Title = "坏", Content = "" },
            ],
        }, now.AddHours(1));

        PromptLibraryDocument loaded = store.Load();
        Assert.Equal(2, result.ImportedPrompts);
        Assert.Equal(1, result.SkippedPrompts);
        Assert.Equal(1, result.ImportedCategories);
        Assert.Equal(1, result.SkippedCategories);
        Assert.Equal(2, loaded.Prompts.Count);
        Assert.Contains(loaded.Prompts, p => p.Id == "p1" && p.Title == "更新");
        Assert.Contains(loaded.Prompts, p => p.Id != "p1" && p.Title == "新增");
        Assert.Contains(loaded.Categories, c => c.Id == "missing" && c.Name == "missing");
    }

    [Fact]
    public void ExportJson_UsesCamelCaseDocumentShape()
    {
        var store = new PromptLibraryStore(_file);
        store.UpsertPrompt(new PromptLibraryEntry { Id = "p1", Title = "标题", Content = "内容" });

        string json = store.ExportJson();

        Assert.Contains("\"version\"", json);
        Assert.Contains("\"categories\"", json);
        Assert.Contains("\"prompts\"", json);
        Assert.Contains("\"categoryId\"", json);
        Assert.DoesNotContain("\"CategoryId\"", json);
    }

    [Fact]
    public void ImportJson_AcceptsExportedDocumentShape()
    {
        var store = new PromptLibraryStore(_file);

        PromptLibraryImportResult result = store.ImportJson("""
            {
              "version": 1,
              "categories": [{ "id": "legal", "name": "法律", "enabled": true }],
              "prompts": [{ "id": "p1", "title": "标题", "content": "内容", "categoryId": "legal", "enabled": true }]
            }
            """);

        Assert.Equal(1, result.ImportedPrompts);
        Assert.Equal(1, result.ImportedCategories);
        Assert.Contains(store.Load().Prompts, p => p.Id == "p1" && p.CategoryId == "legal");
    }
}
