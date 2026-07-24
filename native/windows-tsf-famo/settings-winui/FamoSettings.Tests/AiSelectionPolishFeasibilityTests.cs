using Xunit;

namespace Famo.Settings.Tests;

public sealed class AiSelectionPolishFeasibilityTests
{
    [Fact]
    public void SelectionPolish_DeepLinkRemainsImplementedButTrayMenuStaysClean()
    {
        string menuPatches =
            File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/status-bar.patch")) +
            File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/tray-options.patch")) +
            File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/features/language-bar-menu.patch"));
        string app = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/App.xaml.cs"));

        Assert.DoesNotContain("AI 润色选中", menuPatches);
        Assert.DoesNotContain("ID_WEASELTRAY_FAMO_AI_POLISH", menuPatches);
        Assert.DoesNotContain(@"launch_famo_settings(dir, L""ai-polish"")", menuPatches);
        Assert.Contains("IsAiPolishPage", app);
        Assert.Contains("ShowAiSelectionPolish", app);
        Assert.Contains("\"ai-polish\"", app);
        Assert.Contains("AiSelectionPolishWindow", app);
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
