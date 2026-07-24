using System.Text.Json;
using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

public class SchemaValidationTests
{
    [Fact]
    public void DefaultSettingsJson_PassesSchema()
    {
        SchemaValidationResult result = SchemaValidator.Validate(SettingsStore.DefaultSettingsJson);
        Assert.True(result.IsValid, $"SCHEMA FAIL famo-settings.default.json:\n{string.Join("\n", result.Errors)}");
        // 证据锚点：dotnet test 通过即 "SCHEMA PASS famo-settings.json"。
    }

    [Fact]
    public void DefaultModel_RoundTripsAndPassesSchema()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        SchemaValidationResult result = SchemaValidator.Validate(settings);
        Assert.True(result.IsValid, string.Join("\n", result.Errors));
    }

    [Fact]
    public void DefaultSettings_RimeIceFullSetPlusWubi()
    {
        // 雾凇 rime-ice 全套 + KyleBing 极点五笔，雾凇拼音优先（首项 rime_ice）+ fontPoint 出厂 19。
        FamoSettings s = SettingsStore.CreateDefault();
        string[] expected =
        {
            "rime_ice", "wubi86_jidian", "double_pinyin_flypy", "t9",
            "wubi86_jidian_pinyin", "wubi86_jidian_trad", "wubi86_jidian_trad_pinyin",
            "double_pinyin", "double_pinyin_mspy", "double_pinyin_sogou",
            "double_pinyin_abc", "double_pinyin_ziguang", "double_pinyin_jiajia",
        };
        Assert.Equal(expected, s.Engine.SchemaList.Select(x => x.Id).ToArray());
        Assert.All(s.Engine.SchemaList, x => Assert.True(x.Enabled));
        Assert.Equal(19, s.Appearance.FontPoint);
        Assert.True(s.Convenience.ShiftSwitch);
        Assert.True(s.Convenience.GoodOldCapsLock);
        Assert.True(s.Convenience.PageMinusEquals);
    }

    [Fact]
    public void FontPointOutOfRange_FailsSchema()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Appearance.FontPoint = 99; // 超出 schema 11–22
        SchemaValidationResult result = SchemaValidator.Validate(settings);
        Assert.False(result.IsValid);
        Assert.NotEmpty(result.Errors);
    }

    [Fact]
    public void UnknownSkinEnum_FailsSchema()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Appearance.Skin = "harvard"; // 不在 enum
        SchemaValidationResult result = SchemaValidator.Validate(settings);
        Assert.False(result.IsValid);
    }

    [Fact]
    public void MissingRequiredField_FailsSchema()
    {
        // 删掉 required 的 engine，应判失败。
        const string json = """{ "version": 3, "appearance": null }""";
        SchemaValidationResult result = SchemaValidator.Validate(json);
        Assert.False(result.IsValid);
    }

    [Fact]
    public void AdditionalProperty_FailsSchema()
    {
        // additionalProperties:false —— 注入未知字段应判失败。
        var node = System.Text.Json.Nodes.JsonNode.Parse(SettingsStore.DefaultSettingsJson)!;
        node["surprise"] = "x";
        SchemaValidationResult result = SchemaValidator.Validate(node.ToJsonString());
        Assert.False(result.IsValid);
    }
}
