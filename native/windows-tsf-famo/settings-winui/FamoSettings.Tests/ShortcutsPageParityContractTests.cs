using Xunit;

namespace Famo.Settings.Tests;

public sealed class ShortcutsPageParityContractTests
{
    [Fact]
    public void ShortcutsPage_ContainsOnlyShortcutVariantGroups()
    {
        string page = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/ShortcutsPage.cs"));

        Assert.Contains("中英文状态切换", page);
        Assert.Contains("Shift 切换中英文", page);
        Assert.Contains("Caps Lock 切换西文", page);
        Assert.Contains("候选翻页快捷键", page);
        Assert.Contains("减号等号翻页", page);
        Assert.Contains("左右中括号翻页", page);
        Assert.Contains("逗号句号翻页", page);
        Assert.Contains("候选快速选词", page);
        Assert.Contains("分号引号选 2 / 3 位", page);
        Assert.Contains("App.SaveAndApplyDeploy();", page);

        Assert.DoesNotContain("中 / 英文标点", page);
        Assert.DoesNotContain("输入方式", page);
        Assert.DoesNotContain("双拼方案", page);
        Assert.DoesNotContain("五笔方案", page);
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
