using Famo.Settings.Core.Legal;
using Xunit;

namespace Famo.Settings.Tests;

/// <summary>
/// 跳库深链契约。URL 形态字节级对齐 macOS FamoVerificationRoutingTests 的真机验收样本——
/// 这些不是从实现回抄的期望值，是对真实站点验证过的检索 URL。
/// </summary>
public sealed class LegalSearchRouterContractTests
{
    private static LegalSearchSite Site(string id) =>
        LegalSearchRouter.Sites.Single(s => s.Id == id);

    [Fact]
    public void CnkiAndBaiduScholar_CarryTheQueryInTheirOwnSearchUrls()
    {
        // 与 macOS 同一 fixture：惩罚性赔偿研究（对真实知网/百度学术验证过带词直达落结果页）。
        const string Query = "惩罚性赔偿研究";
        const string Encoded = "%E6%83%A9%E7%BD%9A%E6%80%A7%E8%B5%94%E5%81%BF%E7%A0%94%E7%A9%B6";

        Assert.Equal(
            $"https://kns.cnki.net/kns8s/defaultresult/index?kw={Encoded}",
            LegalSearchRouter.SearchUrl(Site("cnki"), Query));
        Assert.Equal(
            $"https://xueshu.baidu.com/s?wd={Encoded}",
            LegalSearchRouter.SearchUrl(Site("baidu-scholar"), Query));
    }

    [Fact]
    public void PaywalledOrJsFrontendSites_FallBackToBingSiteSearch()
    {
        // 北大法宝（付费）与国家法规库（JS 前端检索）自家搜索框带不了词 → 必应 site: 落结果页。
        string pkulaw = LegalSearchRouter.SearchUrl(Site("pkulaw"), "民法典第一百四十三条");
        string govlaw = LegalSearchRouter.SearchUrl(Site("govlaw"), "民法典第一百四十三条");

        Assert.StartsWith("https://www.bing.com/search?q=site%3Apkulaw.com%20", pkulaw);
        Assert.StartsWith("https://www.bing.com/search?q=site%3Aflk.npc.gov.cn%20", govlaw);
        Assert.Contains("%E6%B0%91%E6%B3%95%E5%85%B8", pkulaw);
    }

    [Fact]
    public void PlusSign_IsPercentEncodedSoMultiTermLegalQueriesDoNotTearApart()
    {
        // 裸 + 会被百度/必应解成空格，撕裂多词法律检索（Responsay issue 328 踩坑修复）。
        string url = LegalSearchRouter.SearchUrl(Site("cnki"), "民法典+侵权责任");

        Assert.Contains("%2B", url);
        Assert.DoesNotContain("+", url);
    }

    [Fact]
    public void NormalizeSelection_CollapsesWhitespaceAndCapsAtEighty()
    {
        Assert.Equal(
            "《民法典》 第一百四十三条 规定",
            LegalSearchRouter.NormalizeSelection("《民法典》\n  第一百四十三条\t 规定"));
        // 超长 CJK 检索词会让站内引擎直接返回空 → 截断到 80。
        Assert.Equal(
            LegalSearchRouter.MaxQueryLength,
            LegalSearchRouter.NormalizeSelection(new string('法', 200)).Length);
    }

    [Fact]
    public void PolishWindow_WiresTheJumpRowForRetrievalSkillsOnly()
    {
        string window = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/settings-winui/FamoSettings/Views/AiSelectionPolishWindow.cs"));

        Assert.Contains("跳库检索", window);
        Assert.Contains("LegalSearchRouter", window);
        // 一律 ShellExecute 开用户默认浏览器，绝不用应用内 webview（对齐 macOS 铁律）。
        Assert.Contains("UseShellExecute = true", window);
        Assert.Contains("\"source-check\" or \"research-assist\"", window);
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
