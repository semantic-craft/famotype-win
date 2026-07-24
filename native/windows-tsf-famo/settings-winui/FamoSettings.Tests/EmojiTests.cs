using Famo.Settings.Core.Emoji;
using Xunit;

namespace Famo.Settings.Tests;

/// <summary>表情面板数据/逻辑核心（对齐 macOS FamoEmojiData / FamoEmojiRecents）。</summary>
public class EmojiDataTests
{
    [Fact]
    public void Tables_PortedSizes_MatchMacOS()
    {
        Assert.Equal(42, FamoEmojiData.Emoji.Count);
        Assert.Equal(40, FamoEmojiData.Symbol.Count);
        Assert.Equal(16, FamoEmojiData.Kaomoji.Count);
        Assert.Equal(24, FamoEmojiData.Punct.Count);
    }

    [Fact]
    public void Kaomoji_AllWide_OthersNarrow()
    {
        Assert.All(FamoEmojiData.Kaomoji, g => Assert.True(g.Wide));
        Assert.All(FamoEmojiData.Emoji, g => Assert.False(g.Wide));
        Assert.All(FamoEmojiData.Punct, g => Assert.False(g.Wide));
    }

    [Fact]
    public void ItemsFor_Recent_IsEmpty()
    {
        Assert.Empty(FamoEmojiData.ItemsFor(FamoEmojiCategory.Recent));
        Assert.NotEmpty(FamoEmojiData.ItemsFor(FamoEmojiCategory.Emoji));
    }

    [Theory]
    [InlineData("笑", "😀")]          // 关键词命中（中文）
    [InlineData("rocket", "🚀")]      // 关键词命中（英文，大小写无关原样）
    [InlineData("ROCKET", "🚀")]      // 大小写不敏感
    [InlineData("书名号", "《》")]     // 跨到标点分类
    public void Search_MatchesByKeyword(string query, string expectedChar)
    {
        var hits = FamoEmojiData.Search(query);
        Assert.Contains(hits, g => g.Char == expectedChar);
    }

    [Fact]
    public void Search_ByChar_FindsGlyph()
    {
        var hits = FamoEmojiData.Search("→");
        Assert.Contains(hits, g => g.Char == "→");
    }

    [Theory]
    [InlineData("")]
    [InlineData("   ")]
    public void Search_BlankQuery_ReturnsEmpty(string query)
    {
        Assert.Empty(FamoEmojiData.Search(query));
    }

    [Fact]
    public void CategoryInfo_LabelsAndGlyphs_Present()
    {
        Assert.Equal("最近", FamoEmojiCategoryInfo.Label(FamoEmojiCategory.Recent));
        Assert.Equal("颜文字", FamoEmojiCategoryInfo.Label(FamoEmojiCategory.Kaomoji));
        Assert.Equal("😀", FamoEmojiCategoryInfo.Glyph(FamoEmojiCategory.Emoji));
    }
}

/// <summary>「最近」store —— 去重 / cap14 / 持久化 / 起始集回退。</summary>
public class EmojiRecentsStoreTests : IDisposable
{
    private readonly string _dir;
    private readonly string _file;

    public EmojiRecentsStoreTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "famo-emoji-" + Guid.NewGuid().ToString("N"));
        _file = Path.Combine(_dir, "emoji-recent.json");
    }

    public void Dispose()
    {
        if (Directory.Exists(_dir)) Directory.Delete(_dir, recursive: true);
    }

    [Fact]
    public void Current_WhenMissing_ReturnsSeed()
    {
        var store = new EmojiRecentsStore(_file);
        Assert.False(File.Exists(_file));
        Assert.Contains("😂", store.Current());
        Assert.Equal(12, store.Current().Count); // 起始集 12 项
    }

    [Fact]
    public void Push_InsertsFront_AndPersists()
    {
        var store = new EmojiRecentsStore(_file);
        store.Push("🔥");
        Assert.Equal("🔥", store.Current()[0]);
        // 新建 store 读同一文件 —— 持久化成功
        Assert.Equal("🔥", new EmojiRecentsStore(_file).Current()[0]);
    }

    [Fact]
    public void Push_Dedupes_MovesToFront()
    {
        var store = new EmojiRecentsStore(_file);
        store.Push("🔥");
        store.Push("✨");
        var list = store.Push("🔥"); // 已存在 → 移到最前，不重复
        Assert.Equal("🔥", list[0]);
        Assert.Single(list, c => c == "🔥");
    }

    [Fact]
    public void Push_CapsAt14()
    {
        var store = new EmojiRecentsStore(_file);
        for (int i = 0; i < 30; i++) store.Push("x" + i);
        Assert.Equal(14, store.Current().Count);
        Assert.Equal("x29", store.Current()[0]); // 最后压入在最前
    }

    [Fact]
    public void Glyphs_WideInferredByGraphemeCount()
    {
        var store = new EmojiRecentsStore(_file);
        store.Push("(≧▽≦)"); // 颜文字 字素 > 3 → wide
        var glyphs = store.Glyphs();
        Assert.True(glyphs[0].Wide);
        // 单 emoji / 短标点不 wide
        Assert.False(new EmojiRecentsStore(_file2()).Glyphs()[0].Wide);
    }

    // 起始集首项 "😂" 用于非 wide 断言。
    private string _file2()
    {
        string p = Path.Combine(_dir, "seed.json");
        return p; // 不存在 → Current() 回退起始集，首项 😂
    }
}

public sealed class EmojiPanelWinuiContractTests
{
    [Fact]
    public void WinuiDeepLink_ShowsEmojiWindowWithoutOpeningMainWindow()
    {
        string app = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/App.xaml.cs"));
        string program = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Program.cs"));

        Assert.Contains("private static EmojiWindow? _emojiWindow;", app);
        Assert.Contains("string.Equals(page, \"emoji\", StringComparison.OrdinalIgnoreCase)", app);
        Assert.Contains("_emojiWindow ??= new EmojiWindow();", app);
        Assert.Contains("_emojiWindow.ShowNearCursor();", app);
        Assert.Contains("if (IsEmojiPage(startPage))", app);
        Assert.Contains("ShowEmoji(); // 只显浮窗", app);
        Assert.Contains("return;", app);
        Assert.Contains("if (IsEmojiPage(page))", app);
        Assert.Contains("ShowEmoji(); // 表情浮窗", app);
        Assert.Contains("WritePendingPage(GetPageArg(Environment.GetCommandLineArgs()))", program);
    }

    [Fact]
    public void EmojiWindow_UsesNoActivateRecentStoreAndUnicodeInjection()
    {
        string window = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/EmojiWindow.cs"));
        string injector = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Interop/TextInjector.cs"));

        Assert.Contains("private readonly EmojiRecentsStore _recents = new();", window);
        Assert.Contains("FamoEmojiData.Search(_search.Text)", window);
        Assert.Contains("_category == FamoEmojiCategory.Recent ? _recents.Glyphs()", window);
        Assert.Contains("_recents.Push(ch);", window);
        Assert.Contains("TextInjector.Inject(ch);", window);
        Assert.Contains("WS_EX_NOACTIVATE", window);
        Assert.Contains("WS_EX_TOOLWINDOW", window);
        Assert.Contains("SetWindowLong(hwnd, GWL_EXSTYLE, ex | WS_EX_TOOLWINDOW)", window);
        Assert.Contains("SetWindowLong(hwnd, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE)", window);

        Assert.Contains("text.Length * 2", injector);
        Assert.Contains("KEYEVENTF_UNICODE", injector);
        Assert.Contains("wScan = scan", injector);
        Assert.Contains("SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<INPUT>())", injector);
    }

    [Fact]
    public void WeaselPopupPanel_DoesNotExposeEmojiEntry()
    {
        string apply = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/apply-famo-statusbar.ps1"));
        string menuPatches =
            File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/status-bar.patch")) +
            File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/tray-options.patch")) +
            File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/language-bar-menu.patch"));

        Assert.DoesNotContain("features/emoji-entry.patch", apply);
        Assert.DoesNotContain("L\"表情符号\"", menuPatches);
        Assert.DoesNotContain("launch_famo_emoji(dir)", menuPatches);
        Assert.DoesNotContain(@"launch_famo_settings(dir, L""emoji"")", menuPatches);
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
