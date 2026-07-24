using Xunit;

namespace Famo.Settings.Tests;

public sealed class AboutPageParityContractTests
{
    [Fact]
    public void AboutPage_OwnsMaintenanceActionsPreviouslyInDeployPage()
    {
        string page = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/AboutPage.cs"));

        Assert.Contains("维护与诊断", page);
        Assert.Contains("刷新配置", page);
        Assert.Contains("DeployService.TriggerReload(ReloadKind.FullDeploy)", page);
        Assert.Contains("FamoPaths.FamoDir", page);
        Assert.Contains("rime-ice", page, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("oh-my-rime", page, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("rime_mint", page, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void AboutPage_ShowsRealVersionAndOpensConfigFolder()
    {
        string page = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/AboutPage.cs"));

        // R1: real assembly version, no unsupported "already latest" claim, no hardcoded literal.
        Assert.Contains("AssemblyInformationalVersionAttribute", page);
        Assert.DoesNotContain("已是最新版本", page);
        Assert.DoesNotContain("v0.1.0", page);

        // R2: config directory row opens Explorer instead of only displaying a read-only path.
        Assert.Contains("UseShellExecute = true", page);
        Assert.Contains("Process.Start", page);
    }

    [Fact]
    public void AboutPage_ExposesHonestManualUpdateCheck()
    {
        string page = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/AboutPage.cs"));
        string menuTest = File.ReadAllText(RepoFile("native/windows-tsf-famo/weasel-fork/tests/Test-FamoMenuParity.ps1"));

        Assert.Contains("软件更新", page);
        Assert.Contains("检查更新", page);
        Assert.Contains("ReleasesUrl", page);
        Assert.Contains("semantic-craft/famotype-win/releases/latest", page);
        Assert.Contains("已打开法墨发布页", page);
        Assert.DoesNotContain("已是最新版本", page);
        Assert.Contains("tray menu must keep update checks in the About page", menuTest);
    }

    [Fact]
    public void AboutPage_ProductCopyIsFamoFirst()
    {
        string page = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/AboutPage.cs"));
        string productCopy = ProductCopy(page);

        Assert.Contains("法墨输入法 · Windows", productCopy);
        Assert.Contains("Famo Input Method", productCopy);
        Assert.Contains("FamoTsf.dll / FamoRuntime.exe", productCopy);
        Assert.DoesNotContain("Weasel", productCopy, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("小狼毫", productCopy);
        Assert.DoesNotContain("weasel.dll", productCopy, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void AboutPage_OpenSourceSectionKeepsProvenance()
    {
        string page = File.ReadAllText(RepoFile("native/windows-tsf-famo/settings-winui/FamoSettings/Views/AboutPage.cs"));
        string notices = OpenSourceCopy(page);

        Assert.Contains("开源与第三方声明", notices);
        Assert.Contains("Weasel", notices);
        Assert.Contains("GPL-3.0", notices);
        Assert.Contains("librime", notices, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("RIME", notices);
        Assert.Contains("THIRD-PARTY-NOTICES", notices);
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

    private static string ProductCopy(string page)
    {
        int legalAt = page.IndexOf("开源与第三方声明", StringComparison.Ordinal);
        Assert.True(legalAt > 0, "About page must split product copy before the open-source section");
        return page.Substring(0, legalAt);
    }

    private static string OpenSourceCopy(string page)
    {
        int legalAt = page.IndexOf("开源与第三方声明", StringComparison.Ordinal);
        Assert.True(legalAt > 0, "About page must include an open-source section");
        return page.Substring(legalAt);
    }
}
