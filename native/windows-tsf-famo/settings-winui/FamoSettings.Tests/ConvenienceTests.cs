using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

/// <summary>键盘便利项 → default.custom.yaml 生成（对齐 macOS FamoRimePatchBuilder）+ 固定项防回归。</summary>
public class ConvenienceTests
{
    private static string Build(System.Action<ConvenienceSettings>? configure = null)
    {
        FamoSettings s = SettingsStore.CreateDefault();
        configure?.Invoke(s.Convenience);
        return ConfigWriter.BuildDefaultCustom(s);
    }

    // ── 固定项防回归：保存部署设置不得丢失去F4 / Shift / Caps / 默认翻页 ──
    [Fact]
    public void Always_EmitsSwitcherHotkeysEmpty_ShiftCaps_AndMinusEqualsPaging()
    {
        string yaml = Build();
        Assert.Contains("switcher/hotkeys: []", yaml);
        Assert.Contains("ascii_composer/switch_key/Shift_L: commit_code", yaml);
        Assert.Contains("ascii_composer/switch_key/Shift_R: commit_code", yaml);
        Assert.Contains("ascii_composer/good_old_caps_lock: true", yaml);
        Assert.Contains("accept: minus, send: Page_Up", yaml);
        Assert.Contains("accept: equal, send: Page_Down", yaml);
    }

    [Fact]
    public void ShiftSwitchOff_DisablesBothShiftKeys()
    {
        string yaml = Build(c => c.ShiftSwitch = false);
        Assert.Contains("ascii_composer/switch_key/Shift_L: noop", yaml);
        Assert.Contains("ascii_composer/switch_key/Shift_R: noop", yaml);
        Assert.DoesNotContain("ascii_composer/switch_key/Shift_L: commit_code", yaml);
        Assert.DoesNotContain("ascii_composer/switch_key/Shift_R: commit_code", yaml);
    }

    [Fact]
    public void GoodOldCapsLockOff_WritesFalseOverride()
    {
        string yaml = Build(c => c.GoodOldCapsLock = false);
        Assert.Contains("ascii_composer/good_old_caps_lock: false", yaml);
        Assert.DoesNotContain("ascii_composer/good_old_caps_lock: true", yaml);
    }

    [Fact]
    public void PageMinusEqualsOff_OmitsMinusEqualsBindings()
    {
        string yaml = Build(c => c.PageMinusEquals = false);
        Assert.DoesNotContain("accept: minus, send: Page_Up", yaml);
        Assert.DoesNotContain("accept: equal, send: Page_Down", yaml);
        Assert.Contains("toggle: ascii_punct", yaml);
    }

    [Fact]
    public void DefaultCustom_DoesNotEmitSchemeSpecificTraditionalShortcut()
    {
        string yaml = Build();
        Assert.DoesNotContain("toggle: traditionalization", yaml);
        Assert.DoesNotContain("toggle: zh_trad", yaml);
        Assert.DoesNotContain("Control+Shift+4", yaml);
        Assert.DoesNotContain("Control+Shift+dollar", yaml);
    }

    [Fact]
    public void Default_NoConvenienceBlocks()
    {
        string yaml = Build(); // 全部默认 false / 空
        Assert.DoesNotContain("punctuator/half_shape", yaml);
        Assert.DoesNotContain("digit_separators", yaml);
        Assert.DoesNotContain("app_options/", yaml);
        Assert.DoesNotContain("select_first_character", yaml);
    }

    [Fact]
    public void PageBrackets_DisablesWordSelect_AndBindsBrackets()
    {
        string yaml = Build(c => c.PageBrackets = true);
        Assert.Contains("key_binder/select_first_character: \"\"", yaml);
        Assert.Contains("key_binder/select_last_character: \"\"", yaml);
        Assert.Contains("accept: bracketleft, send: Page_Up", yaml);
        Assert.Contains("accept: bracketright, send: Page_Down", yaml);
    }

    [Fact]
    public void PageCommaPeriod_BindsCommaPeriod()
    {
        string yaml = Build(c => c.PageCommaPeriod = true);
        Assert.Contains("accept: comma, send: Page_Up", yaml);
        Assert.Contains("accept: period, send: Page_Down", yaml);
        // 未开 [] 翻页 → 不停用以词定字
        Assert.DoesNotContain("select_first_character", yaml);
    }

    [Fact]
    public void Select23_SendsTwoThree()
    {
        string yaml = Build(c => c.Select23Semicolon = true);
        Assert.Contains("accept: semicolon, send: 2", yaml);
        Assert.Contains("accept: apostrophe, send: 3", yaml);
    }

    [Fact]
    public void SlashToDun_BothPunctuatorTables()
    {
        string yaml = Build(c => c.SlashToDun = true);
        Assert.Contains("\"punctuator/half_shape/+\":", yaml);
        Assert.Contains("\"punctuator/full_shape/+\":", yaml);
        Assert.Contains("\"/\": { commit: \"、\" }", yaml);
    }

    [Fact]
    public void DigitSeparators_EmitsNativeKey()
    {
        string yaml = Build(c => c.DigitSeparators = true);
        Assert.Contains("punctuator/digit_separators: \",.:\"", yaml);
    }

    [Fact]
    public void AppEnglish_OnePerExe_TrimmedAndSkipsBlank()
    {
        string yaml = Build(c => c.AppEnglishExes = new() { "devenv.exe", "  code.exe  ", "" });
        Assert.Contains("\"app_options/devenv.exe/ascii_mode\": true", yaml);
        Assert.Contains("\"app_options/code.exe/ascii_mode\": true", yaml);
        // 空行不生成
        Assert.DoesNotContain("app_options//ascii_mode", yaml);
    }

    [Fact]
    public void AllOn_StillHasSchemaListAndFixedItems()
    {
        string yaml = Build(c =>
        {
            c.PageBrackets = true; c.PageCommaPeriod = true; c.Select23Semicolon = true;
            c.SlashToDun = true; c.DigitSeparators = true; c.AppEnglishExes = new() { "x.exe" };
        });
        Assert.Contains("- schema: rime_ice", yaml);
        Assert.Contains("menu/page_size:", yaml);
        Assert.Contains("switcher/hotkeys: []", yaml);
        Assert.Contains("ascii_composer/switch_key/Shift_R: commit_code", yaml);
    }
}
