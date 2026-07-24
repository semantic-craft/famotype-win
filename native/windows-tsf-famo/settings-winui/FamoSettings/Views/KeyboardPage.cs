using Famo.Settings.Core;
using Famo.Settings.Theming;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Famo.Settings.Views;

/// <summary>键盘输入（键）—— 对齐 macOS 键盘输入页：输入方式、默认状态、五笔、模糊、符号、emoji、App 默认英文。</summary>
public sealed class KeyboardPage : UserControl
{
    private static InputMethodSettings M => App.Settings.InputMethod;
    private static EngineSettings E => App.Settings.Engine;
    private static SwitchSettings S => App.Settings.Switches;
    private static ConvenienceSettings C => App.Settings.Convenience;
    private static WubiSettings W => App.Settings.Engine.Wubi;

    private static readonly (string Label, string Value)[] Methods =
    {
        ("拼音输入", "pinyin"),
        ("双拼输入", "double_pinyin"),
        ("五笔输入", "wubi"),
    };

    private static readonly (string Label, string Value)[] DoublePinyinSchemes =
    {
        ("小鹤双拼", "flypy"),
        ("微软双拼", "mspy"),
        ("自然码双拼", "natural"),
        ("搜狗双拼", "sogou"),
        ("智能ABC双拼", "abc"),
        ("紫光双拼", "ziguang"),
        ("拼音加加双拼", "jiajia"),
    };

    private static readonly (string Label, string Value)[] WubiSchemes =
    {
        ("86 版五笔", "jidian86"),
        ("五笔拼音混输", "pinyinMix"),
    };

    private static readonly (string Label, string Value)[] WubiModes =
    {
        ("默认", "normal"),
        ("单字优先", "single_first"),
        ("纯单字", "single_only"),
    };

    private readonly FrameworkElement _doublePinyinRow;
    private readonly FrameworkElement _wubiSchemeRow;
    private readonly Border _wubiCard;
    private readonly TextBlock _applyStatus;

    public KeyboardPage()
    {
        var sp = new StackPanel();
        sp.Children.Add(FamoUI.PaneHeader("键盘输入", "选择输入方式、默认状态、五笔、模糊拼音、符号与 App 默认英文。"));

        var applyStatusRow = FamoUI.StatusRow("设置会按对应桶即时保存；部署类设置会自动重建索引。");
        _applyStatus = (TextBlock)(applyStatusRow.Child as StackPanel)!.Children[1];
        sp.Children.Add(FamoUI.FilledButton("保存并应用当前配置", "", ReapplyCurrentKeyboardConfig));
        sp.Children.Add(applyStatusRow);

        int methodIdx = IndexOf(Methods, M.Method);
        _doublePinyinRow = FamoUI.Row("双拼方案", "选择双拼键位布局。",
            Combo(DoublePinyinSchemes, M.DoublePinyinLayout, idx =>
            {
                M.DoublePinyinLayout = DoublePinyinSchemes[idx].Value;
                ApplySchema();
            }));
        _wubiSchemeRow = FamoUI.Row("五笔方案", "选择 86 版五笔或五笔拼音混输。",
            Combo(WubiSchemes, M.WubiScheme, idx =>
            {
                M.WubiScheme = WubiSchemes[idx].Value;
                ApplySchema();
            }));

        sp.Children.Add(FamoUI.Card("输入方式",
            FamoUI.Row("方式", "拼音输入、双拼输入、五笔输入；切换后立即应用到当前会话。",
                FamoUI.SegBar(Labels(Methods), methodIdx, OnMethodChanged), divider: false),
            _doublePinyinRow,
            _wubiSchemeRow));

        sp.Children.Add(FamoUI.Card("默认状态",
            FamoUI.Row("中英文", "中文输入或英文直接输入；每个新程序窗口都会回放该默认态。",
                FamoUI.Pill(S.AsciiMode, v => { S.AsciiMode = v; ApplyOption(); }), divider: false),
            FamoUI.Row("简繁体", "简体或繁体输出；同时覆盖拼音、双拼、五笔与五笔拼音混输。",
                FamoUI.Pill(S.Traditionalization, v => { S.Traditionalization = v; ApplyOption(); })),
            FamoUI.Row("全半角", "半角或全角输入。",
                FamoUI.Pill(S.FullShape, v => { S.FullShape = v; ApplyOption(); }))));

        int wubiModeIdx = IndexOf(WubiModes, W.CandidateMode);
        _wubiCard = FamoUI.Card("五笔专属",
            FamoUI.Row("候选编码提示", "在候选旁显示五笔编码，帮助记忆字根。",
                FamoUI.Check(W.CodeHint, v => { W.CodeHint = v; ApplyDeploy(); }), divider: false),
            FamoUI.Row("空码自动清码", "敲出无字的空码时自动清空，免手动退格。",
                FamoUI.Check(W.AutoClear, v => { W.AutoClear = v; ApplyDeploy(); })),
            FamoUI.Row("单字候选", "默认=词字混排；单字优先=单字靠前；纯单字=只出单字。",
                FamoUI.SegBar(Labels(WubiModes), wubiModeIdx, OnWubiModeChanged)),
            FamoUI.Row("z 临时拼音反查", "开=按 z 进入临时拼音反查不会写的字；关=禁用。",
                FamoUI.Check(W.ZReverseLookup, v => { W.ZReverseLookup = v; ApplyDeploy(); })));
        sp.Children.Add(_wubiCard);

        var fz = E.FuzzyPinyin;
        sp.Children.Add(FamoUI.Card("模糊拼音",
            FuzzyRow("zh = z", "翘舌不分", fz.ZhZ, v => { fz.ZhZ = v; ApplyDeploy(); }, divider: false),
            FuzzyRow("ch = c", "翘舌不分", fz.ChC, v => { fz.ChC = v; ApplyDeploy(); }),
            FuzzyRow("sh = s", "翘舌不分", fz.ShS, v => { fz.ShS = v; ApplyDeploy(); }),
            FuzzyRow("n = l", "鼻边音不分（双向）", fz.NL, v => { fz.NL = v; ApplyDeploy(); }),
            FuzzyRow("r = l", "声母混淆", fz.RL, v => { fz.RL = v; ApplyDeploy(); }),
            FuzzyRow("f = h", "声母混淆（双向）", fz.FH, v => { fz.FH = v; ApplyDeploy(); }),
            FuzzyRow("an = ang", "前后鼻音不分（双向）", fz.AnAng, v => { fz.AnAng = v; ApplyDeploy(); }),
            FuzzyRow("en = eng", "前后鼻音不分（双向）", fz.EnEng, v => { fz.EnEng = v; ApplyDeploy(); }),
            FuzzyRow("in = ing", "前后鼻音不分（双向）", fz.InIng, v => { fz.InIng = v; ApplyDeploy(); })));

        sp.Children.Add(FamoUI.Card("符号",
            FamoUI.Row("中文下使用英文标点", "开=输入英文标点（, . ?），关=中文标点（，。？）。",
                FamoUI.Pill(S.AsciiPunct, v => { S.AsciiPunct = v; ApplyOption(); }), divider: false),
            FamoUI.Row("输入中文时 “/?” → “、”", "中文输入时按 / 直接上屏「、」（? 不受影响）。",
                FamoUI.Pill(C.SlashToDun, v => { C.SlashToDun = v; ApplyDeploy(); })),
            FamoUI.Row("数字间标点用半角", "16:00、3.14、1,000 中的 . , : 保持半角。",
                FamoUI.Pill(C.DigitSeparators, v => { C.DigitSeparators = v; ApplyDeploy(); })),
            FamoUI.Row("成对标点自动补全", "打（自动补全）且光标在中间；紧接着打）不会重复。即时生效。",
                FamoUI.Pill(C.AutoPairPunctuation, v => { C.AutoPairPunctuation = v; ApplyInstant(); })),
            FamoUI.Row("中英文之间自动加空格", "打字时在中文与英文字母之间自动插入一个空格。即时生效。",
                FamoUI.Pill(C.CjkEnglishSpacing, v => { C.CjkEnglishSpacing = v; ApplyInstant(); })),
            FamoUI.Row("中文与数字之间自动加空格", "打字时在中文与阿拉伯数字之间自动插入一个空格。即时生效。",
                FamoUI.Pill(C.CjkNumberSpacing, v => { C.CjkNumberSpacing = v; ApplyInstant(); }))));

        sp.Children.Add(FamoUI.Card("候选 emoji",
            FamoUI.Row("在候选中显示 emoji", "打开后，打字时 emoji 会作为候选出现；也可用输入法菜单临时切换。",
                FamoUI.Pill(E.EmojiEnabled, v => { E.EmojiEnabled = v; ApplyOption(); }), divider: false)));

        var appEnglish = AppEnglishCard();
        sp.Children.Add(appEnglish);

        RefreshMethodVisibility();
        Content = sp;
    }

    private void ReapplyCurrentKeyboardConfig()
    {
        ApplySchema();
        ApplyOption();
        ApplyDeploy();
    }

    private void ApplySchema()
    {
        ReloadResult r = App.SaveAndApplySchema();
        App.ReportReloadResult(r, _applyStatus, succeeded: "输入方式已生效。", failedPrefix: "输入方式应用失败");
    }

    private void ApplyOption()
    {
        ReloadResult r = App.SaveAndApplyOption();
        App.ReportReloadResult(r, _applyStatus, succeeded: "默认状态已生效。", failedPrefix: "默认状态应用失败");
    }

    private void ApplyDeploy()
    {
        ReloadResult r = App.SaveAndApplyDeploy();
        App.ReportReloadResult(r, _applyStatus, running: "正在部署键盘配置…", succeeded: "键盘配置已部署生效。", failedPrefix: "键盘配置应用失败");
    }

    private void ApplyInstant()
    {
        ReloadResult r = App.SaveAndApplyInstant();
        App.ReportReloadResult(r, _applyStatus, succeeded: "键盘即时配置已生效。", failedPrefix: "键盘即时配置应用失败");
    }

    private Border AppEnglishCard()
    {
        var input = new TextBox
        {
            PlaceholderText = "devenv.exe",
            MinWidth = 240,
        };

        var status = new TextBlock
        {
            Text = "输入程序 exe 名后添加；列表变更会立即写入 app_options。",
            FontSize = 12.5,
            Foreground = FamoUI.Br("Famo.Ink2"),
            TextWrapping = TextWrapping.Wrap,
        };

        var list = new StackPanel { Spacing = 8, Margin = new Thickness(0, 12, 0, 0) };

        void RenderList()
        {
            list.Children.Clear();
            if (C.AppEnglishExes.Count == 0)
            {
                list.Children.Add(new TextBlock
                {
                    Text = "暂无 App 默认英文条目。",
                    FontSize = 13,
                    Foreground = FamoUI.Br("Famo.Ink2"),
                    Margin = new Thickness(0, 4, 0, 0),
                });
                return;
            }

            foreach (string exe in C.AppEnglishExes)
            {
                list.Children.Add(AppEnglishEntry(exe, () =>
                {
                    C.AppEnglishExes.RemoveAll(x => string.Equals(x, exe, StringComparison.OrdinalIgnoreCase));
                    ApplyDeploy();
                    RenderList();
                    status.Text = $"已移除 {exe}。";
                }));
            }
        }

        void AddEntry()
        {
            string? exe = NormalizeAppEnglishExe(input.Text);
            if (exe is null)
            {
                status.Text = "请输入程序 exe 名。";
                return;
            }

            if (C.AppEnglishExes.Any(x => string.Equals(x, exe, StringComparison.OrdinalIgnoreCase)))
            {
                status.Text = $"{exe} 已在列表中。";
                return;
            }

            C.AppEnglishExes.Add(exe);
            input.Text = "";
            ApplyDeploy();
            RenderList();
            status.Text = $"已添加 {exe}。";
        }

        var add = new Button { Content = "添加", MinWidth = 72 };
        add.Click += (_, _) => AddEntry();

        var inputRow = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        inputRow.Children.Add(input);
        inputRow.Children.Add(add);

        var body = new StackPanel { Spacing = 8 };
        body.Children.Add(inputRow);
        body.Children.Add(status);
        body.Children.Add(list);
        RenderList();

        return FamoUI.Card("App 默认英文", FamoUI.RowFull(body));
    }

    private UIElement AppEnglishEntry(string exe, Action remove)
    {
        var grid = new Grid { ColumnSpacing = 12 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        var name = new TextBlock
        {
            Text = exe,
            FontFamily = FamoUI.Mono,
            FontSize = 13,
            Foreground = FamoUI.Br("Famo.Ink"),
            VerticalAlignment = VerticalAlignment.Center,
        };
        Grid.SetColumn(name, 0);
        grid.Children.Add(name);

        var delete = new Button { Content = "删除", MinWidth = 64 };
        delete.Click += (_, _) => remove();
        Grid.SetColumn(delete, 1);
        grid.Children.Add(delete);

        return new Border
        {
            CornerRadius = new CornerRadius(7),
            Background = FamoUI.Br("Famo.Field"),
            BorderBrush = FamoUI.Br("Famo.Line2"),
            BorderThickness = new Thickness(1),
            Padding = new Thickness(10),
            Child = grid,
        };
    }

    private void OnMethodChanged(int idx)
    {
        M.Method = Methods[idx].Value;
        RefreshMethodVisibility();
        ApplySchema();
    }

    private void OnWubiModeChanged(int idx)
    {
        W.CandidateMode = WubiModes[idx].Value;
        ApplyDeploy();
    }

    private void RefreshMethodVisibility()
    {
        _doublePinyinRow.Visibility = M.Method == "double_pinyin" ? Visibility.Visible : Visibility.Collapsed;
        _wubiSchemeRow.Visibility = M.Method == "wubi" ? Visibility.Visible : Visibility.Collapsed;
        _wubiCard.Visibility = M.Method == "wubi" ? Visibility.Visible : Visibility.Collapsed;
    }

    private static Grid FuzzyRow(string title, string desc, bool on, Action<bool> changed, bool divider = true)
        => FamoUI.Row(title, desc, FamoUI.Check(on, changed), divider);

    private static ComboBox Combo((string Label, string Value)[] items, string value, Action<int> changed)
    {
        var combo = new ComboBox { MinWidth = 168 };
        foreach ((string label, _) in items)
        {
            combo.Items.Add(new ComboBoxItem { Content = label });
        }
        combo.SelectedIndex = IndexOf(items, value);
        combo.SelectionChanged += (_, _) =>
        {
            if (combo.SelectedIndex >= 0)
            {
                changed(combo.SelectedIndex);
            }
        };
        return combo;
    }

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

    private static string? NormalizeAppEnglishExe(string raw)
    {
        string name = Path.GetFileName(raw.Trim().Trim('"'));
        if (string.IsNullOrWhiteSpace(name))
        {
            return null;
        }

        return name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? name : name + ".exe";
    }
}
