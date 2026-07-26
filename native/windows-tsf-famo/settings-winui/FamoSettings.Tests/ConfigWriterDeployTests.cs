using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

/// <summary>部署桶写入：store → default.custom.yaml（schema_list/page_size）+ rime_ice.custom.yaml（switches/模糊音）。</summary>
public class ConfigWriterDeployTests
{
    [Fact]
    public void DefaultCustom_EmitsEnabledSchemasInOrderPlusPageSize()
    {
        FamoSettings s = SettingsStore.CreateDefault(); // 雾凇全套 + 五笔，全 enabled，page_size 8
        string yaml = ConfigWriter.BuildDefaultCustom(s);

        Assert.Contains("- schema: rime_ice", yaml);
        Assert.Contains("- schema: wubi86_jidian_trad_pinyin", yaml);
        Assert.Contains("menu/page_size: 8", yaml);
        // 第一项应是 rime_ice（默认，雾凇拼音优先）。
        int idxIce = yaml.IndexOf("- schema: rime_ice", System.StringComparison.Ordinal);
        int idxWubi = yaml.IndexOf("- schema: wubi86_jidian\n", System.StringComparison.Ordinal);
        Assert.True(idxIce < idxWubi);
    }

    [Fact]
    public void DefaultCustom_SkipsDisabledSchemas()
    {
        FamoSettings s = SettingsStore.CreateDefault();
        s.Engine.SchemaList.First(x => x.Id == "wubi86_jidian_trad").Enabled = false;
        s.Engine.PageSize = 9;
        string yaml = ConfigWriter.BuildDefaultCustom(s);

        Assert.DoesNotContain("- schema: wubi86_jidian_trad\n", yaml);
        // 但 _trad_pinyin 仍在（确保未误删相近名）。
        Assert.Contains("- schema: wubi86_jidian_trad_pinyin", yaml);
        Assert.Contains("menu/page_size: 9", yaml);
    }

    [Fact]
    public void RimeIce_DefaultReset_EmojiOff()
    {
        FamoSettings s = SettingsStore.CreateDefault(); // emoji off
        string yaml = ConfigWriter.BuildRimeIceCustom(s);

        // emoji reset 0（法墨出厂关）。
        Assert.Matches(@"(?s)- name:\s*emoji\b.*?reset:\s*0", yaml);
        Assert.Contains("states: [ 中, 英 ]", yaml);
        Assert.Contains("states: [ 👍, 😄 ]", yaml);
        // tone_display 已下线，不应再有该 active switch（注释里提一句不算）。
        Assert.DoesNotMatch(@"name:\s*tone_display", yaml);
        // 无模糊音时不追加实际 derive 规则。
        Assert.DoesNotContain("derive/zh/z/", yaml);
    }

    [Fact]
    public void RimeIce_EmojiOn_FlipsReset()
    {
        FamoSettings s = SettingsStore.CreateDefault();
        s.Engine.EmojiEnabled = true;
        string yaml = ConfigWriter.BuildRimeIceCustom(s);
        Assert.Matches(@"(?s)- name:\s*emoji\b.*?reset:\s*1", yaml);
    }

    [Fact]
    public void RimeIce_FuzzyToggles_AppendAnchoredDeriveRules()
    {
        FamoSettings s = SettingsStore.CreateDefault();
        s.Engine.FuzzyPinyin.ZhZ = true;
        s.Engine.FuzzyPinyin.ShS = true;
        s.Engine.FuzzyPinyin.InIng = true;
        string yaml = ConfigWriter.BuildRimeIceCustom(s);

        Assert.Contains("\"speller/algebra/+\":", yaml);
        Assert.Contains("derive/^zh/z/", yaml);
        Assert.Contains("derive/^sh/s/", yaml);
        // 韵母对双向发两条。
        Assert.Contains("derive/in$/ing/", yaml);
        Assert.Contains("derive/ing$/in/", yaml);
        // 未勾选的对不应出现。
        Assert.DoesNotContain("derive/^ch/c/", yaml);
        Assert.DoesNotContain("derive/^n/l/", yaml);
    }

    [Fact]
    public void RimeIce_NoFuzzy_OmitsAlgebraBlock()
    {
        FamoSettings s = SettingsStore.CreateDefault(); // 9 对全 false
        string yaml = ConfigWriter.BuildRimeIceCustom(s);
        // 模板自身注释已含 speller/algebra/+ 与 derive 示例；这里断言「未追加」勾选块（其唯一标记）。
        Assert.DoesNotContain("# 模糊音（设置面板勾选", yaml);
        Assert.DoesNotContain("speller/algebra/+\":\n    - derive", yaml);
    }

    [Theory]
    [InlineData("n_l", "derive/^n/l/", "derive/^l/n/")]
    [InlineData("f_h", "derive/^f/h/", "derive/^h/f/")]
    [InlineData("an_ang", "derive/an$/ang/", "derive/ang$/an/")]
    [InlineData("en_eng", "derive/en$/eng/", "derive/eng$/en/")]
    [InlineData("in_ing", "derive/in$/ing/", "derive/ing$/in/")]
    public void RimeIce_BidirectionalPairs_EmitBothDirections(string pair, string fwd, string rev)
    {
        FamoSettings s = SettingsStore.CreateDefault();
        FuzzyPinyinSettings f = s.Engine.FuzzyPinyin;
        switch (pair)
        {
            case "n_l": f.NL = true; break;
            case "f_h": f.FH = true; break;
            case "an_ang": f.AnAng = true; break;
            case "en_eng": f.EnEng = true; break;
            case "in_ing": f.InIng = true; break;
        }
        string yaml = ConfigWriter.BuildRimeIceCustom(s);
        Assert.Contains(fwd, yaml);
        Assert.Contains(rev, yaml);
    }

    [Theory]
    [InlineData("r_l", "derive/^r/l/")]
    [InlineData("zh_z", "derive/^zh/z/")]
    [InlineData("ch_c", "derive/^ch/c/")]
    [InlineData("sh_s", "derive/^sh/s/")]
    public void RimeIce_OneWayPairs_EmitSingleDerive(string pair, string rule)
    {
        FamoSettings s = SettingsStore.CreateDefault();
        FuzzyPinyinSettings f = s.Engine.FuzzyPinyin;
        switch (pair)
        {
            case "r_l": f.RL = true; break;
            case "zh_z": f.ZhZ = true; break;
            case "ch_c": f.ChC = true; break;
            case "sh_s": f.ShS = true; break;
        }
        string yaml = ConfigWriter.BuildRimeIceCustom(s);
        Assert.Contains(rule, yaml);
    }

    // ─────────────── 五笔专属 wubi86_jidian.custom.yaml ───────────────

    [Fact]
    public void Wubi_Defaults_StripsComment_NoLua_KeepsReverse()
    {
        FamoSettings s = SettingsStore.CreateDefault(); // CodeHint/AutoClear off, normal, zReverse on
        string yaml = ConfigWriter.BuildWubiCustom(s);

        Assert.StartsWith("#", yaml);
        Assert.Contains("patch:", yaml);
        // 编码提示关 → 抹注释
        Assert.Contains("\"translator/comment_format\":", yaml);
        Assert.Contains("- xform/.+//", yaml);
        // 空码清码关 → 空串
        Assert.Contains("\"speller/auto_clear\": \"\"", yaml);
        // normal → 不发 lua filter
        Assert.DoesNotContain("lua_filter@", yaml);
        // z 反查默认 on → 不清 recognizer
        Assert.DoesNotContain("recognizer/patterns/reverse_lookup", yaml);
    }

    [Fact]
    public void Wubi_CodeHintOn_KeepsRawCodes()
    {
        FamoSettings s = SettingsStore.CreateDefault();
        s.Engine.Wubi.CodeHint = true;
        string yaml = ConfigWriter.BuildWubiCustom(s);
        Assert.Contains("\"translator/comment_format\": []", yaml);
        Assert.DoesNotContain("xform/.+//", yaml);
    }

    [Fact]
    public void Wubi_AutoClearOn_EmitsMaxLength()
    {
        FamoSettings s = SettingsStore.CreateDefault();
        s.Engine.Wubi.AutoClear = true;
        string yaml = ConfigWriter.BuildWubiCustom(s);
        Assert.Contains("\"speller/auto_clear\": max_length", yaml);
    }

    [Theory]
    [InlineData("single_first", "lua_filter@*wubi86_jidian_single_char_first_filter")]
    [InlineData("single_only", "lua_filter@*wubi86_jidian_single_char_only")]
    public void Wubi_CandidateMode_AppendsLuaFilter(string mode, string lua)
    {
        FamoSettings s = SettingsStore.CreateDefault();
        s.Engine.Wubi.CandidateMode = mode;
        string yaml = ConfigWriter.BuildWubiCustom(s);
        Assert.Contains("\"engine/filters/+\":", yaml);
        Assert.Contains(lua, yaml);
    }

    [Fact]
    public void Wubi_ZReverseOff_ClearsRecognizer()
    {
        FamoSettings s = SettingsStore.CreateDefault();
        s.Engine.Wubi.ZReverseLookup = false;
        string yaml = ConfigWriter.BuildWubiCustom(s);
        Assert.Contains("\"recognizer/patterns/reverse_lookup\": \"\"", yaml);
    }

    // base 合并：保留出厂 schema/icon 品牌键（部署不冲掉五笔托盘图标）。
    [Fact]
    public void Wubi_MergesIntoBase_PreservesBrandIconKeys()
    {
        const string seeded =
            "# wubi86_jidian.custom.yaml — 法墨品牌托盘图标补丁\n" +
            "patch:\n" +
            "  \"schema/icon\": famo_zh.ico\n" +
            "  \"schema/ascii_icon\": famo_ascii.ico\n";
        FamoSettings s = SettingsStore.CreateDefault();
        s.Engine.Wubi.CandidateMode = "single_first";

        string yaml = ConfigWriter.BuildWubiCustom(s, seeded);

        // 出厂图标键保留。
        Assert.Contains("\"schema/icon\": famo_zh.ico", yaml);
        Assert.Contains("\"schema/ascii_icon\": famo_ascii.ico", yaml);
        // 仍有一个 patch: 头，五笔块也写入。
        Assert.Contains("lua_filter@*wubi86_jidian_single_char_first_filter", yaml);
        Assert.Single(System.Text.RegularExpressions.Regex.Matches(yaml, "^patch:", System.Text.RegularExpressions.RegexOptions.Multiline));
    }

    // 幂等：重复部署同一文件不重复追加 engine/filters/+（lua 只出现一次）。
    [Fact]
    public void Wubi_Reapply_IsIdempotent_NoDuplicateLuaFilter()
    {
        const string seeded =
            "# 品牌\npatch:\n  \"schema/icon\": famo_zh.ico\n";
        FamoSettings s = SettingsStore.CreateDefault();
        s.Engine.Wubi.CandidateMode = "single_only";

        string once = ConfigWriter.BuildWubiCustom(s, seeded);
        string twice = ConfigWriter.BuildWubiCustom(s, once);

        Assert.Single(System.Text.RegularExpressions.Regex.Matches(twice, "lua_filter@\\*wubi86_jidian_single_char_only"));
        Assert.Contains("\"schema/icon\": famo_zh.ico", twice);
        // 二次写与一次写稳定一致。
        Assert.Equal(once, twice);
    }
}
