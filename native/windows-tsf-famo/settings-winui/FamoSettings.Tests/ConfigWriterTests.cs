using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

/// <summary>S4.1-write（即时桶）：store → weasel.custom.yaml style/*，preset 色板原样保留。</summary>
public class ConfigWriterTests : IDisposable
{
    private readonly string _dir;

    public ConfigWriterTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "famo-cw-" + Guid.NewGuid().ToString("N"));
    }

    public void Dispose()
    {
        if (Directory.Exists(_dir)) Directory.Delete(_dir, recursive: true);
    }

    [Fact]
    public void Default_WritesExpectedStyleAndPreservesPresets()
    {
        FamoSettings s = SettingsStore.CreateDefault(); // shenda / system / 19pt / horizontal
        string yaml = ConfigWriter.BuildWeaselCustom(s);

        Assert.Contains("color_scheme: shenda", yaml);
        Assert.Contains("color_scheme_dark: shenda_dark", yaml);
        Assert.Contains("font_point: 19", yaml);
        Assert.Contains("horizontal: true", yaml);
        // preset 色板必须原样保留（BGR 十六进制不被破坏）。
        Assert.Contains("preset_color_schemes/shenda:", yaml);
        Assert.Contains("0x532CA8", yaml);
        Assert.Contains("preset_color_schemes/xiada_dark:", yaml);
    }

    [Fact]
    public void Wuda_Vertical_Dark_MapsCorrectly()
    {
        FamoSettings s = SettingsStore.CreateDefault();
        s.Appearance.Skin = "wuda";
        s.Appearance.AppearanceMode = "dark";
        s.Appearance.Orientation = "vertical";
        s.Appearance.FontPoint = 23;
        s.Appearance.Layout.CornerRadius = 12;
        string yaml = ConfigWriter.BuildWeaselCustom(s);

        // dark = 强制暗：两个键都指向 _dark。
        Assert.Contains("color_scheme: wuda_dark", yaml);
        Assert.Contains("color_scheme_dark: wuda_dark", yaml);
        Assert.Contains("horizontal: false", yaml);
        Assert.Contains("font_point: 23", yaml);
        Assert.Contains("corner_radius: 12", yaml);
    }

    [Fact]
    public void Light_ForcesLightSchemeForBothKeys()
    {
        FamoSettings s = SettingsStore.CreateDefault();
        s.Appearance.Skin = "stanford";
        s.Appearance.AppearanceMode = "light";
        string yaml = ConfigWriter.BuildWeaselCustom(s);
        Assert.Contains("color_scheme: stanford", yaml);
        Assert.Contains("color_scheme_dark: stanford", yaml); // 强制亮
    }

    [Fact]
    public void WriteInstantBucket_CreatesFileAndIsIdempotent()
    {
        FamoSettings s = SettingsStore.CreateDefault();
        s.Appearance.Skin = "xiada";

        string path = ConfigWriter.WriteInstantBucket(s, _dir);
        Assert.True(File.Exists(path));
        string first = File.ReadAllText(path);
        Assert.Contains("color_scheme: xiada", first);

        // 再写一次（以已存在文件为模板）应稳定，且 preset 仍在。
        ConfigWriter.WriteInstantBucket(s, _dir);
        string second = File.ReadAllText(path);
        Assert.Equal(first, second);
        Assert.Contains("preset_color_schemes/xiada:", second);
    }

    [Fact]
    public void RoundTripThroughExistingFile_PreservesPresetsAndUpdatesStyle()
    {
        FamoSettings s = SettingsStore.CreateDefault();
        ConfigWriter.WriteInstantBucket(s, _dir); // seed file w/ presets

        s.Appearance.Skin = "wuda";
        s.Appearance.FontPoint = 16;
        string path = ConfigWriter.WriteInstantBucket(s, _dir); // update existing
        string yaml = File.ReadAllText(path);

        Assert.Contains("color_scheme: wuda", yaml);
        Assert.Contains("font_point: 16", yaml);
        Assert.Contains("preset_color_schemes/shenda:", yaml); // 所有 8 皮肤色板仍在
        Assert.Contains("0x67832A", yaml);                      // wuda 高亮色值未被破坏
    }

    // ─────────────── 即时外观覆盖层 famo-style.yaml (②) ───────────────

    [Fact]
    public void StyleOverlay_IsPlainStyleMap_NoPatchNoPresets()
    {
        FamoSettings s = SettingsStore.CreateDefault(); // shenda / system / 19pt / horizontal
        string yaml = ConfigWriter.BuildStyleOverlay(s);

        Assert.StartsWith("#", yaml);                 // 头部注释
        Assert.Contains("style:", yaml);
        Assert.Contains("color_scheme: shenda", yaml);
        Assert.Contains("color_scheme_dark: shenda_dark", yaml);
        Assert.Contains("font_point: 19", yaml);
        Assert.Contains("horizontal: true", yaml);
        // 覆盖层是独立 style 映射：既无 patch 包裹，也不含皮肤色板（由 weasel.yaml 提供）。
        Assert.DoesNotContain("patch:", yaml);
        Assert.DoesNotContain("preset_color_schemes", yaml);
    }

    [Fact]
    public void StyleOverlay_Wuda_Vertical_Dark_MapsCorrectly()
    {
        FamoSettings s = SettingsStore.CreateDefault();
        s.Appearance.Skin = "wuda";
        s.Appearance.AppearanceMode = "dark";
        s.Appearance.Orientation = "vertical";
        s.Appearance.FontPoint = 18;
        s.Appearance.Layout.CornerRadius = 12;
        string yaml = ConfigWriter.BuildStyleOverlay(s);

        Assert.Contains("color_scheme: wuda_dark", yaml);
        Assert.Contains("color_scheme_dark: wuda_dark", yaml);
        Assert.Contains("horizontal: false", yaml);
        Assert.Contains("font_point: 18", yaml);
        Assert.Contains("corner_radius: 12", yaml);
    }

    [Fact]
    public void StyleOverlay_EmitsNativeTsfCommitBehaviors()
    {
        FamoSettings s = SettingsStore.CreateDefault();
        s.Convenience.AutoPairPunctuation = true;
        s.Convenience.CjkEnglishSpacing = true;
        s.Convenience.CjkNumberSpacing = true;
        string yaml = ConfigWriter.BuildStyleOverlay(s);
        Assert.Contains("famo_auto_pair: true", yaml);
        Assert.Contains("famo_cjk_english_spacing: true", yaml);
        Assert.Contains("famo_cjk_number_spacing: true", yaml);
    }

    [Fact]
    public void WriteStyleOverlay_CreatesFamoStyleYaml_AndIsIdempotent()
    {
        FamoSettings s = SettingsStore.CreateDefault();
        s.Appearance.Skin = "xiada";

        string path = ConfigWriter.WriteStyleOverlay(s, _dir);
        Assert.True(File.Exists(path));
        Assert.EndsWith("famo-style.yaml", path);
        string first = File.ReadAllText(path);
        Assert.Contains("color_scheme: xiada", first);

        ConfigWriter.WriteStyleOverlay(s, _dir);
        Assert.Equal(first, File.ReadAllText(path));
    }

    // ─────────────── 即时开关 famo-options.yaml (①) ───────────────

    [Fact]
    public void OptionsOverlay_IsOptionsMap_WithSwitchValues()
    {
        FamoSettings s = SettingsStore.CreateDefault(); // 标点中文/半角/简体/emoji off
        string yaml = ConfigWriter.BuildOptionsOverlay(s);

        Assert.StartsWith("#", yaml);
        Assert.Contains("options:", yaml);
        Assert.Contains("ascii_mode: false", yaml);   // 默认输入态：中文（对位 macOS 默认状态）
        Assert.Contains("ascii_punct: false", yaml);
        Assert.Contains("full_shape: false", yaml);
        Assert.Contains("traditionalization: false", yaml);
        Assert.Contains("zh_trad: false", yaml);       // 五笔简繁默认
        Assert.Contains("emoji: false", yaml);
        Assert.DoesNotContain("tone_display", yaml);
        Assert.DoesNotContain("patch:", yaml);
    }

    [Fact]
    public void OptionsOverlay_FlipsWithEngineAndSwitchFlags()
    {
        FamoSettings s = SettingsStore.CreateDefault();
        s.Switches.AsciiMode = true;
        s.Switches.AsciiPunct = true;
        s.Switches.FullShape = true;
        s.Switches.Traditionalization = true;
        s.Engine.EmojiEnabled = true;
        string yaml = ConfigWriter.BuildOptionsOverlay(s);
        Assert.Contains("ascii_mode: true", yaml);
        Assert.Contains("ascii_punct: true", yaml);
        Assert.Contains("full_shape: true", yaml);
        Assert.Contains("traditionalization: true", yaml);
        Assert.Contains("emoji: true", yaml);
    }

    [Fact]
    public void OptionsOverlay_Traditional_AlsoEmitsZhTrad_ForWubi()
    {
        // 繁体默认须同时发 traditionalization(rime-ice) 与 zh_trad(五笔)，否则五笔不生效。
        FamoSettings s = SettingsStore.CreateDefault();
        s.Switches.Traditionalization = true;
        string yaml = ConfigWriter.BuildOptionsOverlay(s);
        Assert.Contains("traditionalization: true", yaml);
        Assert.Contains("zh_trad: true", yaml);
    }

    [Fact]
    public void WriteOptionsOverlay_CreatesFamoOptionsYaml_AndIsIdempotent()
    {
        FamoSettings s = SettingsStore.CreateDefault();
        string path = ConfigWriter.WriteOptionsOverlay(s, _dir);
        Assert.True(File.Exists(path));
        Assert.EndsWith("famo-options.yaml", path);
        string first = File.ReadAllText(path);
        ConfigWriter.WriteOptionsOverlay(s, _dir);
        Assert.Equal(first, File.ReadAllText(path));
    }
}
