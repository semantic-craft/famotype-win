using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class HotKeyBindingTests
{
    [Theory]
    [InlineData("ctrl+alt+j", "Ctrl+Alt+J")]
    [InlineData("Ctrl + Shift + Q", "Ctrl+Shift+Q")]
    [InlineData("Alt+Shift+Z", "Alt+Shift+Z")]
    [InlineData("", "")]
    public void Normalize_AcceptsOnlyCanonicalRestrictedCombos(string input, string expected) =>
        Assert.Equal(expected, HotKeyBinding.Normalize(input));

    [Theory]
    [InlineData("Ctrl+J")]
    [InlineData("Ctrl+Alt+1")]
    [InlineData("Win+Alt+J")]
    [InlineData("Ctrl+Alt+Shift+")]
    public void Normalize_RejectsUnsafeOrMalformedCombos(string input) =>
        Assert.Null(HotKeyBinding.Normalize(input));

    [Fact]
    public void Defaults_AreUnsetAndRoundTrip()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        Assert.Equal("", settings.HotKeys.QuickPhrasePanel);
        Assert.Equal("", settings.HotKeys.SelectionToolbox);
    }
}
