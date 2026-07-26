using Famo.Settings.Core;
using Famo.Settings.Theming;
using Microsoft.UI.Xaml.Controls;

namespace Famo.Settings.Views;

/// <summary>候选窗设置（候）—— 即时桶为主：排列/字体/字号/个数/行为/预览。</summary>
public sealed class CandidatePage : UserControl
{
    private static AppearanceSettings A => App.Settings.Appearance;
    private static EngineSettings E => App.Settings.Engine;

    private static readonly string[] FontSizes = { "小", "中", "大" };
    private static readonly int[] FontPoints = { 14, 16, 19 };
    private static readonly (string Label, string Value)[] CandidateFormats =
    {
        ("标准", "full"),
        ("隐藏注释", "no_comment"),
        ("仅候选", "candidate_only"),
    };

    // R1：预览宿主与候选个数应用状态需要在多个即时桶handler之间保活，故为实例状态。
    private readonly ContentControl _previewHost;
    private readonly Border _pageSizeStepper;
    private readonly TextBlock _pageSizeStatus;

    public CandidatePage()
    {
        var sp = new StackPanel();
        sp.Children.Add(FamoUI.PaneHeader("候选窗设置", "候选窗的排列、字体、字号与行为。本页改动实时生效，无需刷新配置。"));
        sp.Children.Add(FamoUI.Banner(false, "本页改动实时生效，无需刷新配置"));

        int dir = A.Orientation == "vertical" ? 2 : 1; // 自动/横排/竖排
        int sizeIdx = Math.Max(0, Array.IndexOf(FontPoints, NearestPoint((int)A.FontPoint)));
        int formatIdx = IndexOf(CandidateFormats, A.CandidateFormat);

        _pageSizeStepper = FamoUI.Stepper(Math.Clamp(E.PageSize, 3, 9), 3, 9, OnPageSizeChanged);

        sp.Children.Add(FamoUI.Card("候选显示",
            FamoUI.Row("排列方向", "候选横排或竖排。",
                FamoUI.SegBar(new[] { "自动", "横排", "竖排" }, dir, i =>
                {
                    A.Orientation = i == 2 ? "vertical" : "horizontal";
                    ReloadResult result = App.SaveAndApplyInstant();
                    ReportInstantResult(result);
                    RefreshPreview();
                }), divider: false),
            FamoUI.Row("候选字体", "候选词使用的字体族。",
                FamoUI.SegBar(new[] { "默认", "衬线", "圆体" }, FontFaceIndex(),
                    i => { A.FontFace = i switch { 1 => "宋体", 2 => "圆体", _ => "微软雅黑" }; ReloadResult result = App.SaveAndApplyInstant(); ReportInstantResult(result); RefreshPreview(); })),
            FamoUI.Row("候选字号", "候选词文字大小。",
                FamoUI.SegBar(FontSizes, sizeIdx, i => { A.FontPoint = FontPoints[i]; ReloadResult result = App.SaveAndApplyInstant(); ReportInstantResult(result); RefreshPreview(); })),
            FamoUI.Row("候选格式", "标准=标签+候选+注释；隐藏注释=标签+候选；仅候选=隐藏标签与注释。",
                FamoUI.SegBar(Labels(CandidateFormats), formatIdx, i =>
                {
                    A.CandidateFormat = CandidateFormats[i].Value;
                    ReloadResult result = App.SaveAndApplyInstant();
                    ReportInstantResult(result);
                    RefreshPreview();
                })),
            FamoUI.Row("候选个数", "一屏候选词个数（menu/page_size）。改后立即生效。", _pageSizeStepper)));

        var pageSizeStatusRow = FamoUI.StatusRow("候选个数改动即时应用，无需手动部署。");
        _pageSizeStatus = (TextBlock)(pageSizeStatusRow.Child as StackPanel)!.Children[1];
        sp.Children.Add(pageSizeStatusRow);

        sp.Children.Add(FamoUI.Card("候选行为",
            FamoUI.Row("内嵌预编辑", "开=拼音同时显示在输入处；候选窗输入串由下一项独立控制。",
                FamoUI.Pill(A.InlinePreedit, v => { A.InlinePreedit = v; ReloadResult result = App.SaveAndApplyInstant(); ReportInstantResult(result); RefreshPreview(); }), divider: false),
            FamoUI.Row("候选窗显示输入串", "显示原始字母、活动音节和可用左右键移动的光标。",
                FamoUI.Pill(A.ShowPreedit, v => { A.ShowPreedit = v; ReloadResult result = App.SaveAndApplyInstant(); ReportInstantResult(result); RefreshPreview(); })),
            FamoUI.Row("预览后页", "横排候选条下方淡显后面一/两页；无序号、不可点，翻页键行为不变。",
                FamoUI.Pill(A.PreviewPages, v => { A.PreviewPages = v; ReloadResult result = App.SaveAndApplyInstant(); ReportInstantResult(result); RefreshPreview(); })),
            FamoUI.Row("预览行数", "预览后续一页或两页。",
                FamoUI.SegBar(new[] { "1 行", "2 行" }, Math.Clamp(A.PreviewRows, 1, 2) - 1,
                    i => { A.PreviewRows = i + 1; ReloadResult result = App.SaveAndApplyInstant(); ReportInstantResult(result); RefreshPreview(); })),
            FamoUI.Row("内嵌候选预览", "开=光标处显示当前候选词的实际文字；关=光标处显示拼音本身（区别于内嵌预编辑，那是控制显示的位置，这个控制显示的内容）。",
                FamoUI.Pill(A.InlineCandidatePreview, v => { A.InlineCandidatePreview = v; ReloadResult result = App.SaveAndApplyInstant(); ReportInstantResult(result); RefreshPreview(); }))));

        sp.Children.Add(FamoUI.Card("候选窗形态",
            FamoUI.Row("圆角", "映射 style/corner_radius，实时改变候选窗外框。",
                FamoUI.Stepper(Math.Clamp(A.Layout.CornerRadius, 0, 16), 0, 16, v =>
                {
                    A.Layout.CornerRadius = v;
                    ReloadResult result = App.SaveAndApplyInstant();
                    ReportInstantResult(result);
                    RefreshPreview();
                }), divider: false),
            FamoUI.Row("内边距", "映射 style/margin_x 与 style/margin_y，实时改变候选窗留白。",
                FamoUI.Stepper(Math.Clamp(A.Layout.Margin, 4, 24), 4, 24, v =>
                {
                    A.Layout.Margin = v;
                    ReloadResult result = App.SaveAndApplyInstant();
                    ReportInstantResult(result);
                    RefreshPreview();
                }))));

        sp.Children.Add(FamoUI.Card("Windows 悬浮状态栏",
            FamoUI.Row("显示悬浮状态条", "语言栏菜单可打开 Windows 独有的悬浮状态条。",
                FamoUI.Value("已接入"), divider: false),
            FamoUI.Row("状态按钮", "中英、标点、简繁、全半角按钮直达当前会话的 Rime option。",
                FamoUI.Value("即时生效")),
            FamoUI.Row("三点菜单", "输入法设定放在最上面；刷新配置放在输入法设定的维护与诊断里；输入区技能不放在这里。",
                FamoUI.ActionButton("打开", () => App.OpenSettingsPage("status-bar")))));

        _previewHost = new ContentControl { Content = FamoPreview.Build(), HorizontalContentAlignment = Microsoft.UI.Xaml.HorizontalAlignment.Center };
        var preview = FamoUI.Card("实时预览", FamoUI.RowFull(_previewHost));
        sp.Children.Add(preview);

        Content = sp;
    }

    // R1：即时显示/行为/形态控件每次应用后，原地重建预览。
    private void RefreshPreview() => _previewHost.Content = FamoPreview.Build();

    // R3：即时桶（排列/字体/字号/格式/预编辑/候选预览）保存失败时，把真实结果写进候选个数状态行。
    private void ReportInstantResult(ReloadResult result)
    {
        App.ReportReloadResult(
            result,
            _pageSizeStatus,
            pending: "候选窗改动已发送应用命令。",
            running: "正在应用候选窗改动…",
            succeeded: "候选窗改动已生效。",
            failedPrefix: "候选窗应用失败");
    }

    // R2：候选个数改的是部署桶，写盘 + 触发部署期间禁用步进器，并把真实结果（而非无脑成功）写进状态行。
    private void OnPageSizeChanged(int v)
    {
        E.PageSize = v;
        _pageSizeStepper.Opacity = 0.5;
        _pageSizeStepper.IsHitTestVisible = false;
        _pageSizeStatus.Text = "正在应用候选个数…";
        ReloadResult result = App.SaveAndApplyDeploy();
        App.ReportReloadResult(
            result,
            _pageSizeStatus,
            pending: "候选个数已保存，并已发送应用命令。",
            running: "正在应用候选个数…",
            succeeded: "候选个数已生效。",
            failedPrefix: "候选个数应用失败");
        _pageSizeStepper.Opacity = 1;
        _pageSizeStepper.IsHitTestVisible = true;
    }

    private static int FontFaceIndex() => A.FontFace switch { "宋体" => 1, "圆体" => 2, _ => 0 };

    private static string[] Labels((string Label, string Value)[] items)
    {
        var r = new string[items.Length];
        for (int i = 0; i < items.Length; i++) r[i] = items[i].Label;
        return r;
    }

    private static int IndexOf((string Label, string Value)[] items, string value)
    {
        for (int i = 0; i < items.Length; i++)
            if (items[i].Value == value) return i;
        return 0;
    }

    private static int NearestPoint(int p)
    {
        int best = FontPoints[0], bd = int.MaxValue;
        foreach (int fp in FontPoints) { int d = Math.Abs(fp - p); if (d < bd) { bd = d; best = fp; } }
        return best;
    }
}
