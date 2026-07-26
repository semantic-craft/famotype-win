using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

public class SettingsStoreTests : IDisposable
{
    private readonly string _dir;
    private readonly string _file;

    public SettingsStoreTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "famo-test-" + Guid.NewGuid().ToString("N"));
        _file = Path.Combine(_dir, "famo-settings.json");
    }

    public void Dispose()
    {
        if (Directory.Exists(_dir))
        {
            Directory.Delete(_dir, recursive: true);
        }
    }

    [Fact]
    public void Load_WhenMissing_SeedsFileWithDefaults()
    {
        Assert.False(File.Exists(_file));

        var store = new SettingsStore(_file);
        FamoSettings settings = store.Load();

        Assert.True(File.Exists(_file)); // 首启 seed 落盘
        Assert.Equal(FamoSettings.CurrentVersion, settings.Version);
        Assert.Equal("shenda", settings.Appearance.Skin);
        Assert.Equal("auto", settings.Appearance.Orientation);
        Assert.Equal(8, settings.Engine.PageSize);
        Assert.False(settings.Engine.EmojiEnabled);          // 出厂 emoji 关
        Assert.Equal("rime_ice", settings.Engine.SchemaList[0].Id); // 出厂雾凇拼音优先
        Assert.False(settings.Switches.Traditionalization);  // 出厂简体

        // seed 出的文件自身也应通过 schema。
        SchemaValidationResult v = SchemaValidator.Validate(File.ReadAllText(_file));
        Assert.True(v.IsValid, string.Join("\n", v.Errors));
    }

    [Fact]
    public void SaveThenLoad_RoundTrips()
    {
        var store = new SettingsStore(_file);
        FamoSettings settings = store.Load();

        settings.Appearance.Skin = "wuda";
        settings.Appearance.FontPoint = 18;
        settings.Appearance.Layout.CornerRadius = 12;
        settings.Engine.PageSize = 9;
        settings.Engine.FuzzyPinyin.ZhZ = true;
        store.Save(settings);

        FamoSettings reloaded = new SettingsStore(_file).Load();
        Assert.Equal("wuda", reloaded.Appearance.Skin);
        Assert.Equal(18, reloaded.Appearance.FontPoint);
        Assert.Equal(12, reloaded.Appearance.Layout.CornerRadius);
        Assert.Equal(9, reloaded.Engine.PageSize);
        Assert.True(reloaded.Engine.FuzzyPinyin.ZhZ);
    }

    [Fact]
    public void Load_Existing_DoesNotReseed()
    {
        var store = new SettingsStore(_file);
        FamoSettings first = store.Load();
        first.Appearance.Skin = "xiada";
        store.Save(first);

        FamoSettings second = new SettingsStore(_file).Load();
        Assert.Equal("xiada", second.Appearance.Skin); // 未被默认覆盖
    }

    [Fact]
    public void Load_LegacyFuzzy_MigratesThreeCombosToNinePairs()
    {
        // 旧版 3 组合写盘（升级前的文件形态）：zh_ch_sh + l_n_f_h_r_l 开，an_en_in 关。
        Directory.CreateDirectory(_dir);
        File.WriteAllText(_file, """
        {
          "engine": {
            "fuzzyPinyin": { "zh_ch_sh": true, "an_en_in": false, "l_n_f_h_r_l": true }
          }
        }
        """);

        FuzzyPinyinSettings f = new SettingsStore(_file).Load().Engine.FuzzyPinyin;
        // zh_ch_sh → zh/ch/sh
        Assert.True(f.ZhZ); Assert.True(f.ChC); Assert.True(f.ShS);
        // l_n_f_h_r_l → n/l + f/h + r/l
        Assert.True(f.NL); Assert.True(f.FH); Assert.True(f.RL);
        // an_en_in 关 → 韵母不迁移
        Assert.False(f.AnAng); Assert.False(f.EnEng); Assert.False(f.InIng);
    }

    [Fact]
    public void Load_LegacyFuzzy_MigratesVowelComboToThreePairs()
    {
        Directory.CreateDirectory(_dir);
        File.WriteAllText(_file, """
        {
          "engine": {
            "fuzzyPinyin": { "an_en_in": true }
          }
        }
        """);

        FuzzyPinyinSettings f = new SettingsStore(_file).Load().Engine.FuzzyPinyin;
        Assert.True(f.AnAng);
        Assert.True(f.EnEng);
        Assert.True(f.InIng);
        Assert.False(f.ZhZ);
        Assert.False(f.NL);
    }

    [Fact]
    public void CreateDefault_AiSelectionMenuAndSkillTogglesDefaultTrue()
    {
        FamoSettings settings = SettingsStore.CreateDefault();

        Assert.True(settings.Ai.SelectionMenuEnabled);
        Assert.True(settings.Ai.PolishSkillEnabled);
        Assert.True(settings.Ai.SourceCheckSkillEnabled);
        Assert.True(settings.Ai.ResearchAssistSkillEnabled);
        Assert.True(settings.Ai.AskAnythingSkillEnabled);
        Assert.True(settings.Ai.PublishFormattingSkillEnabled);
        Assert.True(settings.Ai.TranslationSkillEnabled);
        Assert.False(settings.Ai.CloudEnabled); // 未受影响的既有默认值
    }

    [Fact]
    public void Load_OldSettingsFileMissingAiToggles_DefaultsAllTrue()
    {
        // 旧版设置文件形态：只有 cloudEnabled，缺当前动作开关。
        Directory.CreateDirectory(_dir);
        File.WriteAllText(_file, """
        {
          "ai": { "cloudEnabled": true }
        }
        """);

        AiSettings ai = new SettingsStore(_file).Load().Ai;
        Assert.True(ai.CloudEnabled);
        Assert.True(ai.SelectionMenuEnabled);
        Assert.True(ai.PolishSkillEnabled);
        Assert.True(ai.SourceCheckSkillEnabled);
        Assert.True(ai.ResearchAssistSkillEnabled);
        Assert.True(ai.AskAnythingSkillEnabled);
        Assert.True(ai.PublishFormattingSkillEnabled);
        Assert.True(ai.TranslationSkillEnabled);
    }

    [Fact]
    public void Load_V3PreviewPages_MigratesToScrollOrientation()
    {
        Directory.CreateDirectory(_dir);
        File.WriteAllText(_file, """
        {
          "version": 3,
          "appearance": { "orientation": "horizontal", "previewPages": true }
        }
        """);

        FamoSettings settings = new SettingsStore(_file).Load();
        Assert.Equal("scroll", settings.Appearance.Orientation);
        Assert.False(settings.Appearance.PreviewPages);
        Assert.Contains("\"orientation\": \"scroll\"", File.ReadAllText(_file));
    }

    [Fact]
    public void Load_V3DocumentFormattingToggle_MigratesToPublishFormatting()
    {
        Directory.CreateDirectory(_dir);
        File.WriteAllText(_file, """
        {
          "version": 3,
          "ai": { "documentFormattingSkillEnabled": false }
        }
        """);

        FamoSettings settings = new SettingsStore(_file).Load();
        Assert.False(settings.Ai.PublishFormattingSkillEnabled);
        string persisted = File.ReadAllText(_file);
        Assert.Contains("\"publishFormattingSkillEnabled\": false", persisted);
        Assert.DoesNotContain("documentFormattingSkillEnabled", persisted);
    }

    [Theory]
    [InlineData(1, 3)]
    [InlineData(30, 9)]
    public void Load_LegacyPageSize_ClampsToSelectableRange(int legacy, int expected)
    {
        Directory.CreateDirectory(_dir);
        File.WriteAllText(_file, $$"""
        {
          "version": 3,
          "engine": { "pageSize": {{legacy}} }
        }
        """);

        FamoSettings settings = new SettingsStore(_file).Load();
        Assert.Equal(expected, settings.Engine.PageSize);
        Assert.Contains($"\"pageSize\": {expected}", File.ReadAllText(_file));
        Assert.True(SchemaValidator.Validate(settings).IsValid);
    }

    [Theory]
    [InlineData("appearance", "skin", "\"harvard\"")]
    [InlineData("appearance", "fontPoint", "99")]
    public void Load_InvalidEnumOrRange_ThrowsAndBacksUpOriginal(
        string section, string property, string value)
    {
        Directory.CreateDirectory(_dir);
        string original = $$"""
        {
          "{{section}}": { "{{property}}": {{value}} }
        }
        """;
        File.WriteAllText(_file, original);

        InvalidDataException ex = Assert.Throws<InvalidDataException>(
            () => new SettingsStore(_file).Load());

        Assert.Contains("schema", ex.Message);
        Assert.Equal(original, File.ReadAllText(_file));
        Assert.Equal(original, File.ReadAllText(_file + ".bak"));
    }

    [Fact]
    public void Save_InvalidSettings_LeavesExistingFileIntact()
    {
        var store = new SettingsStore(_file);
        FamoSettings settings = store.Load();
        string original = File.ReadAllText(_file);
        settings.Appearance.Skin = "harvard";

        InvalidDataException ex = Assert.Throws<InvalidDataException>(() => store.Save(settings));

        Assert.Contains("schema", ex.Message);
        Assert.Equal(original, File.ReadAllText(_file));
        Assert.False(File.Exists(_file + ".tmp"));
    }

    [Fact]
    public void Load_V1InlinePreeditTrue_MigratesToPopupPreeditAndPersists()
    {
        Directory.CreateDirectory(_dir);
        File.WriteAllText(_file, """
        {
          "version": 1,
          "appearance": { "inlinePreedit": true }
        }
        """);

        FamoSettings settings = new SettingsStore(_file).Load();

        Assert.Equal(FamoSettings.CurrentVersion, settings.Version);
        Assert.False(settings.Appearance.InlinePreedit);
        Assert.Contains("inline_preedit: false", ConfigWriter.BuildWeaselCustom(settings));

        string persisted = File.ReadAllText(_file);
        Assert.Contains("\"version\": 4", persisted);
        Assert.Contains("\"inlinePreedit\": false", persisted);
    }

    [Fact]
    public void Load_V2InlinePreeditTrue_PreservesUserChoice()
    {
        Directory.CreateDirectory(_dir);
        File.WriteAllText(_file, """
        {
          "version": 2,
          "appearance": { "inlinePreedit": true }
        }
        """);

        FamoSettings settings = new SettingsStore(_file).Load();

        Assert.Equal(FamoSettings.CurrentVersion, settings.Version);
        Assert.True(settings.Appearance.InlinePreedit);
    }

    [Fact]
    public void Load_V2FactoryGeometry_MigratesToMacAlignedDefaults()
    {
        Directory.CreateDirectory(_dir);
        File.WriteAllText(_file, """
        {
          "version": 2,
          "appearance": {
            "layout": { "cornerRadius": 8, "borderWidth": 1, "shadowRadius": 4, "margin": 12 }
          }
        }
        """);

        LayoutSettings layout = new SettingsStore(_file).Load().Appearance.Layout;

        Assert.Equal(13, layout.CornerRadius);
        Assert.Equal(16, layout.ShadowRadius);
        Assert.Equal(8, layout.Margin);
    }

    [Fact]
    public void Load_V2CustomizedGeometry_IsPreserved()
    {
        Directory.CreateDirectory(_dir);
        File.WriteAllText(_file, """
        {
          "version": 2,
          "appearance": {
            "layout": { "cornerRadius": 12, "borderWidth": 1, "shadowRadius": 6, "margin": 10 }
          }
        }
        """);

        LayoutSettings layout = new SettingsStore(_file).Load().Appearance.Layout;

        Assert.Equal(12, layout.CornerRadius);
        Assert.Equal(6, layout.ShadowRadius);
        Assert.Equal(10, layout.Margin);
    }

    [Fact]
    public void Load_NewFuzzyFormat_NoSpuriousMigration()
    {
        Directory.CreateDirectory(_dir);
        File.WriteAllText(_file, """
        {
          "engine": {
            "fuzzyPinyin": { "zh_z": true, "an_ang": true }
          }
        }
        """);

        FuzzyPinyinSettings f = new SettingsStore(_file).Load().Engine.FuzzyPinyin;
        Assert.True(f.ZhZ);
        Assert.True(f.AnAng);
        Assert.False(f.ChC); // 旧键缺失，迁移不应虚开
    }
}
