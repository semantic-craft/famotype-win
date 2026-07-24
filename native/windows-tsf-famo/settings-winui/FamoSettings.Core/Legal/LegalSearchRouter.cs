namespace Famo.Settings.Core.Legal;

/// <summary>一个跳库目标站。<see cref="DirectBase"/> 非空走「带词直达」（该站自己的检索 URL）；
/// 否则走「必应 site:域名 检索词」。</summary>
public sealed record LegalSearchSite(
    string Id,
    string DisplayName,
    string? DirectBase,
    string? DirectParam,
    string? BingDomain);

/// <summary>
/// 法律检索跳库深链：把选中文本变成权威库的检索结果页 URL，本地拼接、零 LLM。
/// 站点与两种深链策略 1:1 移植自 macOS FamoVerificationRouting（真机验收过的 URL 形态）：
/// 知网/百度学术自家检索 URL 带词直达；北大法宝（付费）与国家法规库（JS 前端检索）
/// 自家搜索框带不了词，走必应 site: 落到结果页。打开 URL（用户默认浏览器）归 UI 层。
/// </summary>
public static class LegalSearchRouter
{
    /// <summary>检索词上限：超长 CJK 检索词会让站内引擎直接返回空（Responsay issue 328，
    /// macOS 侧实测钉死 80）。</summary>
    public const int MaxQueryLength = 80;

    private const string BingSearchBase = "https://www.bing.com/search";

    public static readonly IReadOnlyList<LegalSearchSite> Sites =
    [
        new("govlaw", "国家法规库", null, null, "flk.npc.gov.cn"),
        new("pkulaw", "北大法宝", null, null, "pkulaw.com"),
        new("cnki", "知网", "https://kns.cnki.net/kns8s/defaultresult/index", "kw", null),
        new("baidu-scholar", "百度学术", "https://xueshu.baidu.com/s", "wd", null),
    ];

    /// <summary>整段选区 → 检索词：合并空白后截断到上限。</summary>
    public static string NormalizeSelection(string text)
    {
        string joined = string.Join(
            ' ', text.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries));
        return joined.Length <= MaxQueryLength ? joined : joined[..MaxQueryLength];
    }

    /// <summary>site 的检索结果页 URL。<see cref="Uri.EscapeDataString(string)"/> 会把 + 编成
    /// %2B——裸 + 会被百度/必应解成空格，撕裂多词法律检索（Responsay issue 328 踩坑修复，
    /// macOS 侧逐字复刻，此处由 stdlib 天然满足）。</summary>
    public static string SearchUrl(LegalSearchSite site, string query) =>
        site.DirectBase is not null
            ? $"{site.DirectBase}?{site.DirectParam}={Uri.EscapeDataString(query)}"
            : $"{BingSearchBase}?q={Uri.EscapeDataString($"site:{site.BingDomain} {query}")}";
}
