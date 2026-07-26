using Famo.Settings.Core;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;

namespace Famo.Settings.Theming;

/// <summary>候选窗预览 mock：输入串光标、薄雾候选、竖排通栏与后页预览。</summary>
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

        var cw = new StackPanel { Spacing = vertical ? 0 : 7, HorizontalAlignment = HorizontalAlignment.Stretch };
        if (A.ShowPreedit)
        {
            cw.Children.Add(new TextBlock
            {
                Text = "ni|hao", FontFamily = FamoUI.Mono, FontSize = Math.Max(11, fs - 3),
                Foreground = FamoUI.Br("Famo.Ink2"), Margin = new Thickness(vertical ? 10 : 14, 7, 8, 6),
            });
        }

        var cands = new StackPanel { Orientation = vertical ? Orientation.Vertical : Orientation.Horizontal, Spacing = vertical ? 0 : 5, HorizontalAlignment = HorizontalAlignment.Stretch };
        (string n, string w)[] items = { ("1", "你好"), ("2", "您好"), ("3", "拟好"), ("4", "逆号") };
        for (int i = 0; i < items.Length; i++)
        {
            bool hi = i == 0;
            var cell = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5 };
            if (showLabel)
                cell.Children.Add(new TextBlock { Text = items[i].n + (vertical ? "." : ""), FontFamily = FamoUI.Mono, FontSize = vertical ? fs * 0.9 : fs * 0.72, Foreground = hi ? FamoUI.Br("Famo.OnAccent") : FamoUI.Br("Famo.Ink3"), VerticalAlignment = VerticalAlignment.Center });
            cell.Children.Add(new TextBlock { Text = items[i].w, FontFamily = wordFont, FontSize = fs, Foreground = hi ? FamoUI.Br("Famo.OnAccent") : FamoUI.Br("Famo.Ink"), VerticalAlignment = VerticalAlignment.Center });
            cands.Children.Add(new Border
            {
                Background = hi ? FamoUI.Br("Famo.Accent") : new SolidColorBrush(Microsoft.UI.Colors.Transparent),
                CornerRadius = new CornerRadius(Math.Min(8, Math.Max(4, cornerRadius - 1))), Padding = new Thickness(vertical ? 10 : 8, 7, vertical ? 10 : 8, 7),
                Margin = vertical ? new Thickness(4, 0, 4, 0) : new Thickness(0),
                HorizontalAlignment = vertical ? HorizontalAlignment.Stretch : HorizontalAlignment.Left,
                Child = cell,
            });
        }
        cw.Children.Add(cands);

        if (!vertical && A.PreviewPages)
        {
            string[][] rows = { new[] { "你号", "拟好", "霓濠", "泥蚝" }, new[] { "倪皓", "逆豪", "匿好", "睨毫" } };
            for (int row = 0; row < Math.Clamp(A.PreviewRows, 1, 2); row++)
            {
                var preview = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5, Opacity = row == 0 ? 0.45 : 0.30 };
                foreach (string word in rows[row])
                    preview.Children.Add(new TextBlock { Text = word, FontFamily = wordFont, FontSize = fs, Foreground = FamoUI.Br("Famo.Ink"), Margin = new Thickness(8, 7, 8, 7) });
                cw.Children.Add(preview);
            }
        }

        var window = new Border
        {
            Background = FamoUI.Br("Famo.Card"),
            BorderBrush = FamoUI.Br("Famo.Accent12"),
            BorderThickness = new Thickness(borderWidth),
            CornerRadius = new CornerRadius(cornerRadius),
            Padding = vertical ? new Thickness(0, 3, 0, 4) : new Thickness(margin),
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
