using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class FamoThemeContractTests
{
    [Fact]
    public void SystemModeKeepsDistinctLightAndDarkRuntimeSchemes()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Appearance.Skin = "wuda";
        settings.Appearance.AppearanceMode = "system";

        string yaml = ConfigWriter.BuildStyleOverlay(settings);

        Assert.Contains("color_scheme: wuda\n", yaml);
        Assert.Contains("color_scheme_dark: wuda_dark\n", yaml);
    }

    [Fact]
    public void SettingsWindowTracksActualSystemThemeWithoutPinningIt()
    {
        string source = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/settings-winui/FamoSettings/Theming/FamoTheme.cs"));

        Assert.Contains("ElementTheme.Default", source);
        Assert.Contains("ActualThemeChanged += OnActualThemeChanged", source);
        Assert.Contains("root.ActualTheme == ElementTheme.Dark", source);
    }

    private static string RepoFile(string relativePath)
    {
        string? dir = AppContext.BaseDirectory;
        while (!string.IsNullOrEmpty(dir))
        {
            string candidate = Path.Combine(dir, relativePath);
            if (File.Exists(candidate)) return candidate;
            dir = Directory.GetParent(dir)?.FullName;
        }
        throw new FileNotFoundException($"Could not locate {relativePath}");
    }
}
