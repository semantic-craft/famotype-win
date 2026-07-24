using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace Famo.Settings.Theming;

/// <summary>
/// 法墨皮肤主题系统（对标 macOS「Claude Design」8-token 调色板）。
/// 4 皮肤 × 明暗 × 8 token（accent/accentDeep/onAccent/card/card2/ink/ink2/ink3）+ 派生中性色
/// （line/line2/hover/knob/onAccent20）。把命名 <see cref="SolidColorBrush"/> 注入
/// Application.Resources，换肤/明暗时**原地改 Color**，使全窗控件实时重绘（StaticResource 复用同一笔刷对象）。
/// </summary>
public static class FamoTheme
{
    // skin → (light 8 token, dark 8 token)，顺序 accent/accentDeep/onAccent/card/card2/ink/ink2/ink3。
    private static readonly Dictionary<string, (string[] Light, string[] Dark)> Palettes = new()
    {
        ["shenda"] = (
            new[] { "A82C53", "8E2447", "FBF9F5", "FBF9F5", "F3EFE7", "2A2622", "6B6A64", "9A9387" },
            new[] { "E06A8E", "C24E72", "1A1816", "262321", "211E1C", "ECE4D8", "A89E90", "766D62" }),
        ["stanford"] = (
            new[] { "8C1515", "820000", "FBFBFC", "FBFBFC", "F1F2F4", "2E2D29", "53565A", "8A8D90" },
            new[] { "B83A4B", "8C1515", "F2F2F0", "26282C", "212327", "E8EAED", "9DA1A6", "6C7075" }),
        ["wuda"] = (
            new[] { "2A8367", "1F6B52", "F9FAF8", "F8FBF9", "EFF2EE", "282D2A", "565F5A", "8A938E" },
            new[] { "3CA081", "2A8367", "121413", "212423", "1C1E1D", "E5EAE7", "98A19C", "66706B" }),
        ["xiada"] = (
            new[] { "1D4A8C", "123061", "F8FAFC", "F8FAFC", "EFF2F7", "242A36", "5C6A81", "8898AF" },
            new[] { "4879C5", "1D4A8C", "0F141C", "212429", "1B1E22", "E6EAF0", "98A4B8", "66758A" }),
    };

    // 8 token brush 键 + 派生键。值在 EnsureBrushes 创建一次，Apply 时改 Color。
    private static readonly string[] TokenKeys =
        { "Famo.Accent", "Famo.AccentDeep", "Famo.OnAccent", "Famo.Card", "Famo.Card2", "Famo.Ink", "Famo.Ink2", "Famo.Ink3" };

    public static event Action? Changed;

    /// <summary>当前解析后的明暗（light/dark），供页面预览等用。</summary>
    public static bool IsDark { get; private set; }

    // 代码侧资源查找：ResourceDictionary 索引器不搜合并字典，故递归手动搜（{StaticResource} 由框架处理）。
    internal static object? FindRes(ResourceDictionary d, string key)
    {
        if (d.ContainsKey(key)) return d[key];
        foreach (ResourceDictionary md in d.MergedDictionaries)
        {
            object? r = FindRes(md, key);
            if (r != null) return r;
        }
        return null;
    }

    // 笔刷在 FamoBrushes.xaml（合并字典）里定义，递归找出同一对象后原地改 Color。
    internal static SolidColorBrush? Find(ResourceDictionary d, string key) => FindRes(d, key) as SolidColorBrush;

    private static SolidColorBrush Brush(string key) =>
        Find(Application.Current.Resources, key)
        ?? throw new InvalidOperationException($"缺少笔刷资源 {key}（检查 FamoBrushes.xaml 是否已合并）。");

    /// <summary>把 skin+mode 解析为一套调色板并原地写入笔刷；mode=system 跟随应用主题。</summary>
    public static void Apply(string skin, string mode)
    {
        if (!Palettes.TryGetValue(skin, out var pal)) pal = Palettes["shenda"];

        bool dark = mode switch
        {
            "dark" => true,
            "light" => false,
            _ => (Application.Current.RequestedTheme == ApplicationTheme.Dark),
        };
        IsDark = dark;
        string[] t = dark ? pal.Dark : pal.Light;

        for (int i = 0; i < TokenKeys.Length; i++)
            Brush(TokenKeys[i]).Color = Hex(t[i]);

        Color ink = Hex(t[5]);
        Color accent = Hex(t[0]);
        Color onAccent = Hex(t[2]);
        Brush("Famo.Line").Color = WithAlpha(ink, dark ? 0.12 : 0.09);
        Brush("Famo.Line2").Color = WithAlpha(ink, dark ? 0.07 : 0.06);
        Brush("Famo.Hover").Color = WithAlpha(accent, dark ? 0.16 : 0.09);
        Brush("Famo.OnAccent20").Color = WithAlpha(onAccent, 0.20);
        Brush("Famo.Accent12").Color = WithAlpha(accent, 0.12);
        Brush("Famo.Knob").Color = Colors.White;
        Brush("Famo.StatusGreen").Color = Hex("2E7D52");
        Brush("Famo.Ink3_55").Color = WithAlpha(Hex(t[7]), 0.55);
        Brush("Famo.Ink3_35").Color = WithAlpha(Hex(t[7]), 0.35);
        Brush("Famo.Field").Color = WithAlpha(ink, 0.05);
        Brush("Famo.SegBg").Color = WithAlpha(ink, 0.06);
        Brush("Famo.BannerInstantBg").Color = WithAlpha(ink, 0.04);
        Brush("Famo.BannerDeployBg").Color = Mix(accent, Hex(t[3]), 0.11); // accent 11% over card
        Brush("Famo.BannerDeployBorder").Color = WithAlpha(accent, 0.26);

        // 让窗口 light/dark 系统部件（滚动条等）也跟随
        if (Application.Current is App && App.Window?.Content is FrameworkElement root)
            root.RequestedTheme = dark ? ElementTheme.Dark : ElementTheme.Light;

        Changed?.Invoke();
    }

    /// <summary>取某皮肤某明暗的 token 色（供候选窗预览等自绘用）。idx 见 TokenKeys 顺序。</summary>
    public static Color Token(string skin, bool dark, int idx)
    {
        if (!Palettes.TryGetValue(skin, out var pal)) pal = Palettes["shenda"];
        return Hex((dark ? pal.Dark : pal.Light)[idx]);
    }

    public static Color Hex(string hex)
    {
        hex = hex.TrimStart('#');
        byte r = Convert.ToByte(hex.Substring(0, 2), 16);
        byte g = Convert.ToByte(hex.Substring(2, 2), 16);
        byte b = Convert.ToByte(hex.Substring(4, 2), 16);
        return Color.FromArgb(255, r, g, b);
    }

    private static Color WithAlpha(Color c, double a) =>
        Color.FromArgb((byte)Math.Round(a * 255), c.R, c.G, c.B);

    // 把 top 以 a 不透明度叠在 baseColor 上（模拟 CSS color-mix(top a%, base)）。
    private static Color Mix(Color top, Color baseColor, double a)
    {
        byte ch(byte t, byte b) => (byte)Math.Round(t * a + b * (1 - a));
        return Color.FromArgb(255, ch(top.R, baseColor.R), ch(top.G, baseColor.G), ch(top.B, baseColor.B));
    }
}
