using Famo.Settings.Core;
using Famo.Settings.Core.QuickPhrases;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class QuickPhraseStoreTests : IDisposable
{
    private readonly string _dir;
    private readonly string _json;
    private readonly string _table;

    public QuickPhraseStoreTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "famo-quick-" + Guid.NewGuid().ToString("N"));
        _json = Path.Combine(_dir, "quick-phrases.json");
        _table = Path.Combine(_dir, "famo_quick_send.txt");
    }

    public void Dispose()
    {
        if (Directory.Exists(_dir)) Directory.Delete(_dir, recursive: true);
    }

    [Theory]
    [InlineData("fmcs", "常用短语", true)]
    [InlineData("a", "单字编码", true)]
    [InlineData("famo2026", "含数字", true)]
    [InlineData("Fmcs", "大写会规范为小写", true)]
    [InlineData("1abc", "数字开头非法", false)]
    [InlineData("abc-", "符号非法", false)]
    [InlineData("abcdefghijklmnopqrstuvwxyzabcdefg", "超长非法", false)]
    [InlineData("ok", "", false)]
    [InlineData("ok", "含\n换行", false)]
    [InlineData("ok", "含\t制表", false)]
    public void Validate_EnforcesMacParityRules(string code, string text, bool valid)
    {
        Assert.Equal(valid, QuickPhraseStore.Validate(new QuickPhraseEntry { Code = code, Text = text }) is null);
    }

    [Fact]
    public void UpsertDeleteAndPersist_UseCodeAsUniqueKey()
    {
        var store = new QuickPhraseStore(_json);

        store.Upsert(new QuickPhraseEntry { Code = "fmcs", Text = "第一次" });
        store.Upsert(new QuickPhraseEntry { Code = "fmzz", Text = "第二条" });
        store.Upsert(new QuickPhraseEntry { Code = " FMCS ", Text = " 更新后 " });

        IReadOnlyList<QuickPhraseEntry> loaded = new QuickPhraseStore(_json).Load();
        Assert.Equal(new[] { "fmcs", "fmzz" }, loaded.Select(e => e.Code).ToArray());
        Assert.Equal("更新后", loaded[0].Text);

        store.Delete("fmcs");
        Assert.Equal("fmzz", store.Load().Single().Code);
    }

    [Fact]
    public void BuildTableDb_WritesHeaderCodeAndVCodeRows()
    {
        var entries = new[]
        {
            new QuickPhraseEntry { Code = "fmcs", Text = "法墨常用短语" },
            new QuickPhraseEntry { Code = "zz", Text = "作者按" },
        };

        string table = QuickPhraseStore.BuildTableDb(entries);

        Assert.Contains("#@/db_name famo_quick_send", table);
        Assert.Contains("#@/db_type tabledb", table);
        Assert.Contains("法墨常用短语\tfmcs\t100000", table);
        Assert.Contains("法墨常用短语\tvfmcs\t100000", table);
        Assert.Contains("作者按\tzz\t100000", table);
        Assert.Contains("作者按\tvzz\t100000", table);
        Assert.DoesNotContain("#@/db_name\tfamo_quick_send.txt", table);
    }

    [Fact]
    public void BuildTableDb_NormalizesSortsAndDeduplicatesLikeMacQuickSendStore()
    {
        var entries = new[]
        {
            new QuickPhraseEntry { Code = "zz", Text = "作者按" },
            new QuickPhraseEntry { Code = " FMCS ", Text = "旧短语" },
            new QuickPhraseEntry { Code = "fmcs", Text = "法墨常用短语" },
        };

        string table = QuickPhraseStore.BuildTableDb(entries);

        Assert.True(table.IndexOf("法墨常用短语\tfmcs\t100000", StringComparison.Ordinal)
            < table.IndexOf("作者按\tzz\t100000", StringComparison.Ordinal));
        Assert.DoesNotContain("旧短语", table);
        Assert.Contains("法墨常用短语\tvfmcs\t100000", table);
    }

    [Fact]
    public void WriteTableDb_PersistsGeneratedRimeText()
    {
        var store = new QuickPhraseStore(_json);
        store.Upsert(new QuickPhraseEntry { Code = "fmcs", Text = "法墨常用短语" });

        store.WriteTableDb(_table);

        Assert.True(File.Exists(_table));
        Assert.Contains("法墨常用短语\tfmcs\t100000", File.ReadAllText(_table));
    }
}

public sealed class QuickPhraseRimePatchTests
{
    [Fact]
    public void RimeIceCustom_InjectsFamoQuickSendTranslatorIdempotently()
    {
        FamoSettings settings = SettingsStore.CreateDefault();

        string once = ConfigWriter.BuildRimeIceCustom(settings);
        string twice = ConfigWriter.BuildRimeIceCustom(settings, once);

        Assert.Contains("table_translator@famo_quick_send", once);
        Assert.Contains("user_dict: famo_quick_send", once);
        Assert.Contains("dictionary: \"\"", once);
        Assert.Contains("enable_completion: true", once);
        Assert.Contains("enable_sentence: false", once);
        Assert.Contains("initial_quality: 100", once);
        Assert.Single(System.Text.RegularExpressions.Regex.Matches(twice, "table_translator@famo_quick_send"));
    }

    [Fact]
    public void WubiCustom_DoesNotInjectBareQuickSendTranslator()
    {
        FamoSettings settings = SettingsStore.CreateDefault();

        string once = ConfigWriter.BuildWubiCustom(settings);
        string twice = ConfigWriter.BuildWubiCustom(settings, once);

        Assert.Contains("\"translator/comment_format\"", once);
        Assert.DoesNotContain("table_translator@famo_quick_send", once);
        Assert.DoesNotContain("user_dict: famo_quick_send", once);
        Assert.DoesNotContain(">>> famo-quick-send >>>", once);
        Assert.Single(System.Text.RegularExpressions.Regex.Matches(twice, ">>> famo-wubi >>>"));
        Assert.DoesNotContain("table_translator@famo_quick_send", twice);
    }
}

public sealed class QuickPhraseWinuiContractTests
{
    [Fact]
    public void QuickPhrasePage_WritesStoreTableDbAndTriggersDeploy()
    {
        string mainWindow = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/MainWindow.xaml.cs"));
        string page = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/QuickPhrasesPage.cs"));
        string app = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/App.xaml.cs"));
        string picker = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/QuickPhrasePickerWindow.cs"));
        string paths = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings.Core/FamoPaths.cs"));

        Assert.Contains("SettingsNavigation.VisiblePages", mainWindow);
        Assert.Contains("quick-phrases", Famo.Settings.Core.SettingsNavigation.VisiblePages.Select(page => page.Id));
        Assert.Contains(Famo.Settings.Core.SettingsNavigation.VisiblePages, page => page.Id == "quick-phrases" && page.Badge == "短");
        Assert.Contains("QuickPhraseStore", page);
        Assert.Contains("FamoPaths.QuickSendTableFile", page);
        Assert.Contains("_store.WriteTableDb(FamoPaths.QuickSendTableFile)", page);
        Assert.Contains("App.SaveAndApplyDeploy();", page);
        Assert.Contains("App.ShowQuickPhrasePicker()", page);
        Assert.Contains("QuickPhraseStore.Validate", page);
        Assert.Contains("QuickPhrasesFile =>", paths);
        Assert.Contains("QuickSendTableFile =>", paths);

        Assert.Contains("IsQuickPhrasePickerPage", app);
        Assert.Contains("quick-phrase-picker", app);
        Assert.Contains("ShowQuickPhrasePicker", app);
        Assert.Contains("QuickPhrasePickerWindow", app);
        Assert.Contains("TextInsertionServices.ClipboardPasteForForegroundTarget()", app);

        Assert.Contains("QuickPhraseStore", picker);
        Assert.Contains("ITextInsertionService", picker);
        Assert.Contains("InsertAsync(entry.Text", picker);
        Assert.Contains("VirtualKey.Enter", picker);
        Assert.Contains("VirtualKey.Escape", picker);
        Assert.Contains("ShowNearCursor", picker);
    }

    [Fact]
    public void StatusBarPopup_DoesNotExposeQuickPhrasePickerEntry()
    {
        string apply = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/apply-famo-statusbar.ps1"));
        string menuPatches =
            File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/status-bar.patch")) +
            File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/tray-options.patch")) +
            File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/language-bar-menu.patch"));

        Assert.DoesNotContain("features/quick-phrases-entry.patch", apply);
        Assert.DoesNotContain("L\"快捷短语\"", menuPatches);
        Assert.DoesNotContain(@"launch_famo_settings(dir, L""quick-phrase-picker"")", menuPatches);
    }

    private static string RepoFile(string relativePath)
    {
        string? dir = AppContext.BaseDirectory;
        while (!string.IsNullOrEmpty(dir))
        {
            string candidate = Path.Combine(dir, relativePath);
            if (File.Exists(candidate))
            {
                return candidate;
            }

            dir = Directory.GetParent(dir)?.FullName;
        }

        throw new FileNotFoundException($"Could not locate {relativePath}");
    }
}
