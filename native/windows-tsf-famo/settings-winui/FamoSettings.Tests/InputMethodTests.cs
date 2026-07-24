using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

/// <summary>切片② 输入方式：ResolveSchemaId 映射 + WriteSelectSchema 落盘（纯逻辑，不真切引擎）。</summary>
public class InputMethodTests
{
    [Fact]
    public void Resolve_Pinyin_MapsToRimeIce()
    {
        var m = new InputMethodSettings { Method = "pinyin" };
        Assert.Equal("rime_ice", m.ResolveSchemaId());
    }

    [Fact]
    public void Resolve_Wubi_MapsToJidian()
    {
        var m = new InputMethodSettings { Method = "wubi" };
        Assert.Equal("wubi86_jidian", m.ResolveSchemaId());
    }

    [Theory]
    [InlineData("jidian86", "wubi86_jidian")]
    [InlineData("pinyinMix", "wubi86_jidian_pinyin")]
    public void Resolve_Wubi_MapsEachMacScheme(string scheme, string expected)
    {
        var m = new InputMethodSettings { Method = "wubi", WubiScheme = scheme };
        Assert.Equal(expected, m.ResolveSchemaId());
    }

    [Fact]
    public void Resolve_Wubi_UnknownScheme_FallsToJidian86()
    {
        var m = new InputMethodSettings { Method = "wubi", WubiScheme = "bogus" };
        Assert.Equal("wubi86_jidian", m.ResolveSchemaId());
    }

    [Theory]
    [InlineData("flypy", "double_pinyin_flypy")]
    [InlineData("natural", "double_pinyin")]
    [InlineData("mspy", "double_pinyin_mspy")]
    [InlineData("sogou", "double_pinyin_sogou")]
    [InlineData("abc", "double_pinyin_abc")]
    [InlineData("ziguang", "double_pinyin_ziguang")]
    [InlineData("jiajia", "double_pinyin_jiajia")]
    public void Resolve_DoublePinyin_MapsEachLayout(string layout, string expected)
    {
        var m = new InputMethodSettings { Method = "double_pinyin", DoublePinyinLayout = layout };
        Assert.Equal(expected, m.ResolveSchemaId());
    }

    [Fact]
    public void Resolve_DoublePinyin_UnknownLayout_FallsToFlypy()
    {
        var m = new InputMethodSettings { Method = "double_pinyin", DoublePinyinLayout = "bogus" };
        Assert.Equal("double_pinyin_flypy", m.ResolveSchemaId());
    }

    [Fact]
    public void Resolve_UnknownMethod_FallsToPinyin()
    {
        var m = new InputMethodSettings { Method = "bogus" };
        Assert.Equal("rime_ice", m.ResolveSchemaId());
    }

    [Fact]
    public void Default_IsPinyinFlypy()
    {
        FamoSettings s = SettingsStore.CreateDefault();
        Assert.Equal("pinyin", s.InputMethod.Method);
        Assert.Equal("flypy", s.InputMethod.DoublePinyinLayout);
        Assert.Equal("jidian86", s.InputMethod.WubiScheme);
        Assert.Equal("rime_ice", s.InputMethod.ResolveSchemaId());
    }

    [Fact]
    public void WriteSelectSchema_WritesResolvedIdSingleLine()
    {
        FamoSettings s = SettingsStore.CreateDefault();
        s.InputMethod.Method = "double_pinyin";
        s.InputMethod.DoublePinyinLayout = "sogou";

        string dir = Path.Combine(Path.GetTempPath(), $"famo-sel-{Guid.NewGuid():N}");
        try
        {
            string path = ConfigWriter.WriteSelectSchema(s, dir);
            Assert.True(File.Exists(path));
            Assert.Equal("famo-select-schema.txt", Path.GetFileName(path));
            Assert.Equal("double_pinyin_sogou", File.ReadAllText(path).Trim());
        }
        finally { if (Directory.Exists(dir)) Directory.Delete(dir, true); }
    }

    [Fact]
    public void WriteSelectSchema_Wubi_WritesJidian()
    {
        FamoSettings s = SettingsStore.CreateDefault();
        s.InputMethod.Method = "wubi";

        string dir = Path.Combine(Path.GetTempPath(), $"famo-sel-{Guid.NewGuid():N}");
        try
        {
            string path = ConfigWriter.WriteSelectSchema(s, dir);
            Assert.Equal("wubi86_jidian", File.ReadAllText(path).Trim());
        }
        finally { if (Directory.Exists(dir)) Directory.Delete(dir, true); }
    }

    [Fact]
    public void WriteSelectSchema_WubiPinyinMix_WritesPinyinMixSchema()
    {
        FamoSettings s = SettingsStore.CreateDefault();
        s.InputMethod.Method = "wubi";
        s.InputMethod.WubiScheme = "pinyinMix";

        string dir = Path.Combine(Path.GetTempPath(), $"famo-sel-{Guid.NewGuid():N}");
        try
        {
            string path = ConfigWriter.WriteSelectSchema(s, dir);
            Assert.Equal("wubi86_jidian_pinyin", File.ReadAllText(path).Trim());
        }
        finally { if (Directory.Exists(dir)) Directory.Delete(dir, true); }
    }
}
