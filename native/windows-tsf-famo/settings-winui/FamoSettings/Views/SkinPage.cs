using Famo.Settings.Core;
using Famo.Settings.Theming;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Microsoft.UI;
using Windows.UI;

namespace Famo.Settings.Views;

/// <summary>皮肤外观（皮）—— 即时桶：4 学院皮肤 + 外观模式 + 候选窗预览。</summary>
public sealed class SkinPage : UserControl
{
    private static AppearanceSettings A => App.Settings.Appearance;

    private static readonly (string Id, string Name)[] Skins =
        { ("shenda", "荔园红"), ("stanford", "胡佛红"), ("wuda", "珞珈青"), ("xiada", "嘉庚蓝") };

    private readonly StackPanel _skinRow = new() { Orientation = Orientation.Horizontal, Spacing = 12 };
    private readonly TextBlock _applyStatus;

    public SkinPage()
    {
        var sp = new StackPanel();
        sp.Children.Add(FamoUI.PaneHeader("皮肤外观", "四款学院皮肤，明暗跟随系统。整窗与候选窗共用同一套调色板，切换即时重绘。"));
        sp.Children.Add(FamoUI.Banner(false, "皮肤 / 明暗切换实时重绘整窗与候选窗"));

        BuildSkinCards();
        sp.Children.Add(FamoUI.Card("配色皮肤", FamoUI.RowFull(_skinRow)));

        int mode = A.AppearanceMode switch { "light" => 1, "dark" => 2, _ => 0 };
        sp.Children.Add(FamoUI.Card("明暗",
            FamoUI.Row("外观", "跟随系统或手动指定明暗（color_scheme ↔ color_scheme_dark）。",
                FamoUI.SegBar(new[] { "跟随系统", "亮", "暗" }, mode, i =>
                {
                    A.AppearanceMode = i switch { 1 => "light", 2 => "dark", _ => "system" };
                    ApplyInstant();
                    BuildSkinCards(); // 明暗换 → swatch 取对应明暗 accent
                }), divider: false)));

        sp.Children.Add(FamoUI.Card("候选窗预览", FamoUI.RowFull(FamoPreview.Build())));

        var applyStatusRow = FamoUI.StatusRow("皮肤 / 明暗改动即时生效，无需部署。");
        _applyStatus = (TextBlock)(applyStatusRow.Child as StackPanel)!.Children[1];
        sp.Children.Add(applyStatusRow);

        Content = sp;
    }

    private void ApplyInstant()
    {
        ReloadResult r = App.SaveAndApplyInstant();
        App.ReportReloadResult(r, _applyStatus, succeeded: "皮肤 / 明暗已生效。", failedPrefix: "皮肤应用失败");
    }

    private void BuildSkinCards()
    {
        _skinRow.Children.Clear();
        bool dark = FamoTheme.IsDark;
        foreach ((string id, string name) in Skins)
        {
            bool sel = id == A.Skin;
            Color accent = FamoTheme.Token(id, dark, 0);
            var sw = new Border
            {
                Width = 46, Height = 46, CornerRadius = new CornerRadius(12),
                Background = new SolidColorBrush(accent),
                BorderBrush = sel ? FamoUI.Br("Famo.Ink") : new SolidColorBrush(Colors.Transparent),
                BorderThickness = new Thickness(2),
            };
            var label = new TextBlock
            {
                Text = name, FontSize = 12,
                Foreground = sel ? FamoUI.Br("Famo.Ink") : FamoUI.Br("Famo.Ink2"),
                FontWeight = sel ? Microsoft.UI.Text.FontWeights.SemiBold : Microsoft.UI.Text.FontWeights.Normal,
                HorizontalAlignment = HorizontalAlignment.Center,
            };
            var card = new StackPanel { Spacing = 8, Padding = new Thickness(4), HorizontalAlignment = HorizontalAlignment.Center };
            card.Children.Add(sw);
            card.Children.Add(label);
            var wrap = new Border { Child = card };
            wrap.PointerPressed += (_, _) => { A.Skin = id; ApplyInstant(); BuildSkinCards(); };
            _skinRow.Children.Add(wrap);
        }
    }
}
