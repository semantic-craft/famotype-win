using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class SettingsParityContractTests
{
    [Fact]
    public void DefaultStateControls_WriteOptionsOverlayAndReplayOnNewSessions()
    {
        // SwitchesPage.cs was deleted (unreachable orphan); these controls are a 100%
        // duplicate that lives in the canonical reachable page, KeyboardPage.cs.
        string keyboardPage = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/KeyboardPage.cs"));
        string app = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/App.xaml.cs"));
        string patch = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/instant-apply.patch"));

        Assert.Contains("默认状态", keyboardPage);
        Assert.Contains("中英文", keyboardPage);
        Assert.Contains("全半角", keyboardPage);
        Assert.Contains("简繁体", keyboardPage);
        Assert.Contains("S.AsciiMode", keyboardPage);
        Assert.Contains("S.FullShape", keyboardPage);
        Assert.Contains("S.Traditionalization", keyboardPage);
        Assert.Contains("App.SaveAndApplyOption();", keyboardPage);

        FamoSettings s = SettingsStore.CreateDefault();
        s.Switches.AsciiMode = true;
        s.Switches.FullShape = true;
        s.Switches.Traditionalization = true;
        string yaml = ConfigWriter.BuildOptionsOverlay(s);
        Assert.Contains("ascii_mode: true", yaml);
        Assert.Contains("full_shape: true", yaml);
        Assert.Contains("traditionalization: true", yaml);
        Assert.Contains("zh_trad: true", yaml);

        Assert.Contains("ConfigWriter.WriteOptionsOverlay(Settings, FamoPaths.FamoDir)", app);
        Assert.Contains("DeployService.ReloadOptions()", app);
        Assert.Contains("_ApplyFamoOptions(session_id);", patch);
        Assert.Contains("rime_api->set_option(session_id, it.key, !!v);", patch);
    }

    [Fact]
    public void WubiSpecificControls_AreVisibleOnlyForWubiAndDeployBacked()
    {
        // InputMethodPage.cs was deleted (unreachable orphan); the Wubi-specific card
        // is a 100% duplicate that lives in the canonical reachable page, KeyboardPage.cs.
        string inputPage = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/KeyboardPage.cs"));

        Assert.Contains("FamoUI.Card(\"五笔专属\"", inputPage);
        Assert.Contains("W.CodeHint", inputPage);
        Assert.Contains("W.AutoClear", inputPage);
        Assert.Contains("W.CandidateMode", inputPage);
        Assert.Contains("W.ZReverseLookup", inputPage);
        Assert.Contains("_wubiCard.Visibility = M.Method == \"wubi\" ? Visibility.Visible : Visibility.Collapsed;", inputPage);
        Assert.Contains("App.SaveAndApplyDeploy();", inputPage);

        FamoSettings s = SettingsStore.CreateDefault();
        s.Engine.Wubi.CodeHint = true;
        s.Engine.Wubi.AutoClear = true;
        s.Engine.Wubi.CandidateMode = "single_first";
        s.Engine.Wubi.ZReverseLookup = false;
        string yaml = ConfigWriter.BuildWubiCustom(s);
        Assert.Contains("\"translator/comment_format\": []", yaml);
        Assert.Contains("\"speller/auto_clear\": max_length", yaml);
        Assert.Contains("lua_filter@*wubi86_jidian_single_char_first_filter", yaml);
        Assert.Contains("\"recognizer/patterns/reverse_lookup\": \"\"", yaml);
    }

    [Fact]
    public void SymbolAutoReplace_IsExistingDigitSeparatorsSetting()
    {
        // ConveniencePage.cs was deleted (unreachable orphan); this setting's live
        // control is the 100% duplicate that lives in KeyboardPage.cs.
        string conveniencePage = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/KeyboardPage.cs"));

        Assert.Contains("数字间标点", conveniencePage);
        Assert.Contains("C.DigitSeparators", conveniencePage);
        Assert.Contains("App.SaveAndApplyDeploy();", conveniencePage);

        FamoSettings s = SettingsStore.CreateDefault();
        s.Convenience.DigitSeparators = true;
        string yaml = ConfigWriter.BuildDefaultCustom(s);
        Assert.Contains("punctuator/digit_separators: \",.:\"", yaml);
    }

    [Fact]
    public void TraditionalizationButton_CoversEveryDefaultSelectableSchema()
    {
        string assemble = File.ReadAllText(RepoFile("native/windows-tsf-famo/famo-config/assemble-payload.sh"));
        string mapping = File.ReadAllText(RepoFile("native/windows-tsf-famo/famo-config/CONFIG-MAPPING.md"));
        // SwitchesPage.cs was deleted (unreachable orphan); the 简/繁 toggle is a 100%
        // duplicate that lives in the canonical reachable page, KeyboardPage.cs.
        string keyboardPage = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/KeyboardPage.cs"));
        FamoSettings s = SettingsStore.CreateDefault();

        var optionBySchema = new Dictionary<string, string>
        {
            ["rime_ice"] = "traditionalization",
            ["double_pinyin_flypy"] = "traditionalization",
            ["t9"] = "traditionalization",
            ["double_pinyin"] = "traditionalization",
            ["double_pinyin_mspy"] = "traditionalization",
            ["double_pinyin_sogou"] = "traditionalization",
            ["double_pinyin_abc"] = "traditionalization",
            ["double_pinyin_ziguang"] = "traditionalization",
            ["double_pinyin_jiajia"] = "traditionalization",
            ["wubi86_jidian"] = "zh_trad",
            ["wubi86_jidian_pinyin"] = "zh_trad",
            ["wubi86_jidian_trad"] = "zh_trad",
            ["wubi86_jidian_trad_pinyin"] = "zh_trad",
        };

        string[] defaultSchemas = s.Engine.SchemaList.Where(x => x.Enabled).Select(x => x.Id).ToArray();
        Assert.Empty(defaultSchemas.Except(optionBySchema.Keys));

        s.Switches.Traditionalization = true;
        string yaml = ConfigWriter.BuildOptionsOverlay(s);
        Assert.Contains("traditionalization: true", yaml);
        Assert.Contains("zh_trad: true", yaml);

        Assert.Contains("ensure_wubi_pinyin_traditionalization", assemble);
        Assert.Contains("wubi86_jidian_pinyin.schema.yaml", assemble);
        Assert.Contains("simplifier@tradition", assemble);
        Assert.Contains("option_name: zh_trad", assemble);
        Assert.Contains("name: zh_trad", assemble);
        Assert.Contains("wubi86_jidian_pinyin", mapping);
        Assert.Contains("注入 `zh_trad`", mapping);
        Assert.Contains("简体或繁体输出；同时覆盖拼音、双拼、五笔与五笔拼音混输。", keyboardPage);
        Assert.DoesNotContain("关=简体（traditionalization）", keyboardPage);
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
