using Famo.Settings.Core;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;

namespace Famo.Settings.Theming;

/// <summary>候选窗预览 mock：按当前皮肤/明暗/字号/横竖排渲染（预编辑行 + 4 候选，首项 accent 高亮）。</summary>
public static class FamoPreview
{
    public static Border Build()
    {
        var A = App.Settings.Appearance;
        bool vertical = A.Orientation == "vertical";
        double fs = Math.Clamp(A.FontPoint, 11, 22);
        int cornerRadius = Math.Clamp(A.Layout.CornerRadius, 0, 16);
        int borderWidth = Math.Clamp(A.Layout.BorderWidth, 0, 3);
        int margin = Math.Clamp(A.Layout.Margin, 4, 24);
        FontFamily wordFont = string.IsNullOrWhiteSpace(A.FontFace) ? FontFamily.XamlAutoFontFamily : new FontFamily(A.FontFace);

        // 候选格式：full=标签+候选+注释；no_comment=标签+候选；candidate_only=仅候选（对齐 CandidatePage 的说明文案）。
        bool showComment = A.CandidateFormat == "full";
        bool showLabel = A.CandidateFormat != "candidate_only";

        var cw = new StackPanel { Spacing = 7, HorizontalAlignment = HorizontalAlignment.Center };
        if (showComment)
        {
            cw.Children.Add(new TextBlock
            {
                Text = "nǐ hǎo ｜ nihao", FontFamily = FamoUI.Mono, FontSize = 13,
                Foreground = FamoUI.Br("Famo.Ink2"), Margin = new Thickness(3, 0, 3, 0),
            });
        }

        var cands = new StackPanel { Orientation = vertical ? Orientation.Vertical : Orientation.Horizontal, Spacing = vertical ? 3 : 5 };
        (string n, string w)[] items = { ("1", "你好"), ("2", "您好"), ("3", "拟好"), ("4", "逆号") };
        for (int i = 0; i < items.Length; i++)
        {
            bool hi = i == 0;
            var cell = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5 };
            if (showLabel)
                cell.Children.Add(new TextBlock { Text = items[i].n, FontFamily = FamoUI.Mono, FontSize = 11, Foreground = hi ? FamoUI.Br("Famo.OnAccent") : FamoUI.Br("Famo.Ink3"), VerticalAlignment = VerticalAlignment.Center });
            cell.Children.Add(new TextBlock { Text = items[i].w, FontFamily = wordFont, FontSize = fs, Foreground = hi ? FamoUI.Br("Famo.OnAccent") : FamoUI.Br("Famo.Ink"), VerticalAlignment = VerticalAlignment.Center });
            cands.Children.Add(new Border
            {
                Background = hi ? FamoUI.Br("Famo.Accent") : new SolidColorBrush(Microsoft.UI.Colors.Transparent),
                CornerRadius = new CornerRadius(Math.Max(4, cornerRadius - 1)), Padding = new Thickness(11, 6, 11, 6), Child = cell,
            });
        }
        cw.Children.Add(cands);

        var window = new Border
        {
            Background = FamoUI.Br("Famo.Card"),
            BorderBrush = FamoUI.Br("Famo.Accent12"),
            BorderThickness = new Thickness(borderWidth),
            CornerRadius = new CornerRadius(cornerRadius),
            Padding = new Thickness(margin),
            HorizontalAlignment = HorizontalAlignment.Center,
            Child = cw,
        };

        return new Border
        {
            CornerRadius = new CornerRadius(cornerRadius),
            Background = FamoUI.Br("Famo.Field"),
            BorderBrush = FamoUI.Br("Famo.Line"),
            BorderThickness = new Thickness(1),
            Padding = new Thickness(24),
            Child = window,
        };
    }
}
