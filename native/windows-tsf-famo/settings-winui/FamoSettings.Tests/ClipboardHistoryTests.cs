using System.Text.Json;
using Famo.Settings.Core;
using Famo.Settings.Core.Clipboard;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class ClipboardHistoryStoreTests : IDisposable
{
    private readonly string _dir;
    private readonly string _file;

    public ClipboardHistoryStoreTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "famo-clipboard-" + Guid.NewGuid().ToString("N"));
        _file = Path.Combine(_dir, "clipboard-history.json");
    }

    public void Dispose()
    {
        if (Directory.Exists(_dir)) Directory.Delete(_dir, recursive: true);
    }

    [Fact]
    public void AddText_WhenDisabled_DoesNotCaptureOrWrite()
    {
        var store = new ClipboardHistoryStore(_file);

        Assert.False(store.AddText("secret", enabled: false));
        Assert.Empty(store.Load());
        Assert.False(File.Exists(_file));
    }

    [Fact]
    public void AddText_TruncatesDedupesMovesRecentToFrontCapsAtTwentyAndPersists()
    {
        var store = new ClipboardHistoryStore(_file);
        DateTimeOffset now = new(2026, 6, 29, 10, 0, 0, TimeSpan.Zero);

        Assert.True(store.AddText(new string('长', 2105), enabled: true, now));
        ClipboardHistoryEntry longEntry = store.Load()[0];
        Assert.Equal(ClipboardHistoryStore.MaxTextLength, longEntry.Text.Length);
        Assert.Equal(now, longEntry.CreatedAt);

        Assert.True(store.AddText("same", enabled: true, now.AddMinutes(1)));
        Assert.True(store.AddText("other", enabled: true, now.AddMinutes(2)));
        Assert.True(store.AddText("same", enabled: true, now.AddMinutes(3)));
        IReadOnlyList<ClipboardHistoryEntry> deduped = store.Load();
        Assert.Equal("same", deduped[0].Text);
        Assert.Single(deduped, e => e.Text == "same");

        for (int i = 0; i < 25; i++)
        {
            Assert.True(store.AddText("item-" + i, enabled: true, now.AddMinutes(10 + i)));
        }

        IReadOnlyList<ClipboardHistoryEntry> capped = new ClipboardHistoryStore(_file).Load();
        Assert.Equal(ClipboardHistoryStore.MaxEntries, capped.Count);
        Assert.Equal("item-24", capped[0].Text);
        Assert.DoesNotContain(capped, e => e.Text == "item-0");
    }

    [Fact]
    public void AddText_TruncationDoesNotPersistHalfOfAnEmoji()
    {
        var store = new ClipboardHistoryStore(_file);
        string prefix = new('a', ClipboardHistoryStore.MaxTextLength - 1);
        string text = prefix + "😀";

        Assert.True(store.AddText(text, enabled: true));

        ClipboardHistoryEntry saved = Assert.Single(new ClipboardHistoryStore(_file).Load());
        Assert.Equal(prefix, saved.Text);
        Assert.True(saved.Text.Length <= ClipboardHistoryStore.MaxTextLength);
        Assert.DoesNotContain('\uFFFD', saved.Text);
    }

    [Fact]
    public void AddText_TruncationKeepsAWholeEmojiThatFitsBeforeTheLimit()
    {
        var store = new ClipboardHistoryStore(_file);
        string prefix = new('a', ClipboardHistoryStore.MaxTextLength - 2);
        string expected = prefix + "😀";

        Assert.True(store.AddText(expected + "x", enabled: true));

        ClipboardHistoryEntry saved = Assert.Single(new ClipboardHistoryStore(_file).Load());
        Assert.Equal(expected, saved.Text);
        Assert.Equal(ClipboardHistoryStore.MaxTextLength, saved.Text.Length);
        Assert.DoesNotContain('\uFFFD', saved.Text);
    }

    [Fact]
    public void Clear_RemovesPersistedHistory()
    {
        var store = new ClipboardHistoryStore(_file);
        store.AddText("one", enabled: true);
        Assert.NotEmpty(store.Load());

        store.Clear();

        Assert.Empty(store.Load());
        Assert.False(File.Exists(_file));
    }

    [Fact]
    public void Load_InvalidJson_ReturnsEmpty()
    {
        Directory.CreateDirectory(_dir);
        File.WriteAllText(_file, "not json");

        Assert.Empty(new ClipboardHistoryStore(_file).Load());
    }
}

public sealed class ClipboardHistorySettingsContractTests
{
    [Fact]
    public void DefaultAndSchema_ExposeDisabledClipboardSetting()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        Assert.False(settings.Clipboard.Enabled);

        using JsonDocument defaults = JsonDocument.Parse(SettingsStore.DefaultSettingsJson);
        Assert.False(defaults.RootElement.GetProperty("clipboard").GetProperty("enabled").GetBoolean());

        string schemaText = File.ReadAllText(RepoFile("native/windows-tsf-famo/famo-config/famo-settings.schema.json"));
        using JsonDocument schema = JsonDocument.Parse(schemaText);
        JsonElement clipboard = schema.RootElement.GetProperty("properties").GetProperty("clipboard");
        Assert.Contains("enabled", clipboard.GetProperty("required").EnumerateArray().Select(x => x.GetString()));
        Assert.Equal("local-only", clipboard.GetProperty("x-famo-privacy").GetString());
    }

    [Fact]
    public void WinuiSettingsPageAndPanel_AreBackedByStoreAndTextInjector()
    {
        string mainWindow = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/MainWindow.xaml.cs"));
        string app = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/App.xaml.cs"));
        string page = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/ClipboardPage.cs"));
        string window = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/ClipboardWindow.cs"));
        string reader = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Interop/ClipboardReader.cs"));

        Assert.Contains("SettingsNavigation.VisiblePages", mainWindow);
        Assert.Contains(Famo.Settings.Core.SettingsNavigation.VisiblePages, page => page.Id == "clipboard" && page.Badge == "贴");
        Assert.Contains("private static ClipboardWindow? _clipboardWindow;", app);
        Assert.Contains("string.Equals(page, \"clipboard-panel\", StringComparison.OrdinalIgnoreCase)", app);
        Assert.Contains("_clipboardWindow ??= new ClipboardWindow();", app);
        Assert.Contains("_clipboardWindow.ShowNearCursor();", app);

        Assert.Contains("App.Settings.Clipboard.Enabled", page);
        Assert.Contains("ClipboardReader.ReadTextAsync()", page);
        Assert.Contains("_store.AddText(text, App.Settings.Clipboard.Enabled", page);
        Assert.Contains("TextInjector.Inject(entry.Text);", page);
        Assert.Contains("纯本地", page);

        Assert.Contains("WS_EX_NOACTIVATE", window);
        Assert.Contains("WS_EX_TOOLWINDOW", window);
        Assert.Contains("ClipboardReader.ReadTextAsync()", window);
        Assert.Contains("_store.AddText(text, App.Settings.Clipboard.Enabled", window);
        Assert.Contains("TextInjector.Inject(entry.Text);", window);

        Assert.Contains("Windows.ApplicationModel.DataTransfer.Clipboard.GetContent()", reader);
        Assert.Contains("StandardDataFormats.Text", reader);
        Assert.Contains("ExcludeClipboardContentFromMonitorProcessing", reader);
    }

    [Fact]
    public void StatusBarPopup_DoesNotExposeClipboardEntry()
    {
        string apply = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/apply-famo-statusbar.ps1"));
        string menuPatches =
            File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/status-bar.patch")) +
            File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/tray-options.patch")) +
            File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/language-bar-menu.patch"));

        Assert.DoesNotContain("features/clipboard-entry.patch", apply);
        Assert.DoesNotContain("L\"剪贴板历史\"", menuPatches);
        Assert.DoesNotContain("launch_famo_clipboard(dir)", menuPatches);
        Assert.DoesNotContain(@"launch_famo_settings(dir, L""clipboard-panel"")", menuPatches);
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
