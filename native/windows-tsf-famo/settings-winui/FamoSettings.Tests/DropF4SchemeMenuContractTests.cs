using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class DropF4SchemeMenuContractTests
{
    [Fact]
    public void DefaultCustom_DisablesSwitcherHotkeys()
    {
        string generated = ConfigWriter.BuildDefaultCustom(SettingsStore.CreateDefault());
        string overlay = File.ReadAllText(RepoFile("native/windows-tsf-famo/famo-config/overlay/default.custom.yaml"));

        Assert.Contains("switcher/hotkeys: []", generated);
        Assert.Contains("switcher/hotkeys: []", overlay);
        Assert.DoesNotContain("accept: F4", generated, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void SettingsUi_DoesNotExposeRawSchemeList()
    {
        string keyboardPage = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/KeyboardPage.cs"));

        Assert.DoesNotContain("输入方案", keyboardPage);
        Assert.DoesNotContain("BuildSchemeList", keyboardPage);
        Assert.DoesNotContain("E.SchemaList", keyboardPage);
        Assert.DoesNotContain("App.SaveAndApplyDeploy();\r\n            }\r\n        foreach", keyboardPage);
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
