using System.Globalization;
using System.Text.RegularExpressions;

namespace Famo.Settings.Core;

/// <summary>
/// 配置写入层：把设置 store 翻译成 RIME/Weasel 的 *.custom.yaml（CONFIG-MAPPING.md）。
///
/// 本切片实现「即时桶」= weasel.custom.yaml 的 style/*（皮肤/明暗/字体/排版/几何）。
/// 做法：以现有 weasel.custom.yaml 为模板，仅对已知 style 标量行做定点替换，
/// 保留 preset_color_schemes（BGR 十六进制色值，绝不能被 YAML 往返破坏）与注释。
///
/// 「部署桶」(default.custom.yaml / rime_ice.custom.yaml) 与 reload IPC 属 S4（C++ 工具链联调），不在此。
/// </summary>
public static class ConfigWriter
{
    private const string FuzzyBegin = "  # >>> famo-fuzzy >>>（设置面板生成，请勿手改）";
    private const string FuzzyEnd = "  # <<< famo-fuzzy <<<";
    private static readonly Regex FuzzyBlockRx =
        new(@"[ \t]*#\s*>>> famo-fuzzy >>>.*?#\s*<<< famo-fuzzy <<<[^\n]*\r?\n?",
            RegexOptions.Singleline | RegexOptions.Compiled);
    private static readonly Regex LegacyFuzzyBlockRx =
        new(@"[ \t]*# 模糊音（设置面板勾选，追加到既有 algebra 末尾）\r?\n[ \t]*""speller/algebra/\+"":\r?\n(?:[ \t]*- derive/[^\r\n]*(?:\r?\n|$))+",
            RegexOptions.Compiled);

    /// <summary>
    /// 由 store 生成 weasel.custom.yaml 文本（基于 baseYaml 模板做定点替换）。
    /// baseYaml 为 null 时用内置 overlay 模板。
    /// </summary>
    public static string BuildWeaselCustom(FamoSettings settings, string? baseYaml = null)
    {
        string yaml = baseYaml ?? EmbeddedResources.WeaselCustomTemplate;
        AppearanceSettings a = settings.Appearance;

        (string scheme, string schemeDark) = ResolveColorSchemes(a);

        yaml = SetScalar(yaml, "color_scheme", scheme);
        yaml = SetScalar(yaml, "color_scheme_dark", schemeDark);
        yaml = SetScalar(yaml, "font_face", Quote(a.FontFace));
        yaml = SetScalar(yaml, "font_point", Num(a.FontPoint));
        (string labelFormat, string labelPoint, string commentPoint) = ResolveCandidateFormat(a);
        yaml = SetScalar(yaml, "label_format", Quote(labelFormat));
        yaml = SetScalar(yaml, "label_font_point", labelPoint);
        yaml = SetScalar(yaml, "comment_font_point", commentPoint);
        yaml = SetScalar(yaml, "horizontal", a.Orientation == "horizontal" ? "true" : "false");
        yaml = SetScalar(yaml, "inline_preedit", a.InlinePreedit ? "true" : "false");
        yaml = SetScalar(yaml, "show_preedit", a.ShowPreedit ? "true" : "false");
        yaml = SetScalar(yaml, "preview_pages", a.PreviewPages ? "true" : "false");
        yaml = SetScalar(yaml, "preview_rows", Math.Clamp(a.PreviewRows, 1, 2).ToString(CultureInfo.InvariantCulture));
        yaml = SetScalar(yaml, "preedit_type", a.InlineCandidatePreview ? "preview" : "composition");
        yaml = SetScalar(yaml, "corner_radius", a.Layout.CornerRadius.ToString(CultureInfo.InvariantCulture));
        yaml = SetScalar(yaml, "border_width", a.Layout.BorderWidth.ToString(CultureInfo.InvariantCulture));
        yaml = SetScalar(yaml, "shadow_radius", a.Layout.ShadowRadius.ToString(CultureInfo.InvariantCulture));
        yaml = SetScalar(yaml, "margin_x", a.Layout.Margin.ToString(CultureInfo.InvariantCulture));
        yaml = SetScalar(yaml, "margin_y", a.Layout.Margin.ToString(CultureInfo.InvariantCulture));
        return yaml;
    }

    /// <summary>
    /// 即时桶落盘：把生成的 weasel.custom.yaml 写到 famoDir。
    /// 已存在则以其为模板（保留用户/seed 的 presets），否则用内置模板。
    /// 返回写入路径。
    ///
    /// 注意：weasel.custom.yaml 现在是「将来一次 deploy / 全新安装」的回填基线，
    /// **不再是即时显示真相源**。即时外观由 <see cref="WriteStyleOverlay"/> 的 famo-style.yaml
    /// + FamoRuntime --control reload-style 承担（零部署）。
    /// </summary>
    public static string WriteInstantBucket(FamoSettings settings, string famoDir)
    {
        Directory.CreateDirectory(famoDir);
        string path = Path.Combine(famoDir, "weasel.custom.yaml");
        string? baseYaml = File.Exists(path) ? File.ReadAllText(path) : null;
        string yaml = BuildWeaselCustom(settings, baseYaml);

        WriteAtomic(path, yaml);
        return path;
    }

    // ─────────────────────────── 即时外观覆盖层 (②) ───────────────────────────
    // famo-style.yaml 是一份独立、自洽的 `style:` 映射（非 patch）。native runtime 经独立 control 通道
    // 以有界标量解析器装入候选窗自有 FamoSkin；色板名映射到产品内置色板 → 零部署即时重绘。

    /// <summary>由 store 生成 famo-style.yaml 覆盖层文本（一份纯 style 映射，新鲜生成，无需模板）。</summary>
    public static string BuildStyleOverlay(FamoSettings settings)
    {
        AppearanceSettings a = settings.Appearance;
        (string scheme, string schemeDark) = ResolveColorSchemes(a);

        var sb = new System.Text.StringBuilder();
        sb.Append("# famo-style.yaml — 法墨即时外观覆盖层（设置面板生成，请勿手改）。\n");
        sb.Append("# FamoRuntime 经 --control reload-style 重读本文件覆盖候选窗样式，无需部署。\n");
        sb.Append("style:\n");
        sb.Append("  color_scheme: ").Append(scheme).Append('\n');
        sb.Append("  color_scheme_dark: ").Append(schemeDark).Append('\n');
        sb.Append("  font_face: ").Append(Quote(a.FontFace)).Append('\n');
        sb.Append("  font_point: ").Append(Num(a.FontPoint)).Append('\n');
        (string labelFormat, string labelPoint, string commentPoint) = ResolveCandidateFormat(a);
        sb.Append("  label_format: ").Append(Quote(labelFormat)).Append('\n');
        sb.Append("  label_font_point: ").Append(labelPoint).Append('\n');
        sb.Append("  comment_font_point: ").Append(commentPoint).Append('\n');
        sb.Append("  horizontal: ").Append(a.Orientation == "horizontal" ? "true" : "false").Append('\n');
        sb.Append("  inline_preedit: ").Append(a.InlinePreedit ? "true" : "false").Append('\n');
        sb.Append("  show_preedit: ").Append(a.ShowPreedit ? "true" : "false").Append('\n');
        sb.Append("  preview_pages: ").Append(a.PreviewPages ? "true" : "false").Append('\n');
        sb.Append("  preview_rows: ").Append(Math.Clamp(a.PreviewRows, 1, 2)).Append('\n');
        sb.Append("  preedit_type: ").Append(a.InlineCandidatePreview ? "preview" : "composition").Append('\n');
        sb.Append("  corner_radius: ").Append(Int(a.Layout.CornerRadius)).Append('\n');
        sb.Append("  border_width: ").Append(Int(a.Layout.BorderWidth)).Append('\n');
        sb.Append("  shadow_radius: ").Append(Int(a.Layout.ShadowRadius)).Append('\n');
        sb.Append("  margin_x: ").Append(Int(a.Layout.Margin)).Append('\n');
        sb.Append("  margin_y: ").Append(Int(a.Layout.Margin)).Append('\n');
        sb.Append("  famo_auto_pair: ").Append(settings.Convenience.AutoPairPunctuation ? "true" : "false").Append('\n');
        sb.Append("  famo_cjk_english_spacing: ").Append(settings.Convenience.CjkEnglishSpacing ? "true" : "false").Append('\n');
        sb.Append("  famo_cjk_number_spacing: ").Append(settings.Convenience.CjkNumberSpacing ? "true" : "false").Append('\n');
        return sb.ToString();
    }

    /// <summary>覆盖层落盘：把 famo-style.yaml 原子写到 famoDir，返回写入路径。</summary>
    public static string WriteStyleOverlay(FamoSettings settings, string famoDir)
    {
        Directory.CreateDirectory(famoDir);
        string path = Path.Combine(famoDir, "famo-style.yaml");
        WriteAtomic(path, BuildStyleOverlay(settings));
        return path;
    }

    // ─────────────────────────── 即时开关 (①) ───────────────────────────
    // famo-options.yaml 是一份 `options:` 映射。native runtime 经独立 control 通道读入后对各会话
    // set_option，并在新会话(AddSession)回放 → 开关跨 App/重启即时生效且持久，零部署。
    // 与部署桶的 switch reset(rime_ice.custom.yaml) 同源（均出自 EngineSettings/SwitchSettings），互不冲突：
    // famo-options 存在时即时回放胜出；不存在时退回部署桶烤进的默认，互为兜底。

    /// <summary>由 store 生成 famo-options.yaml 文本（运行时可 set_option 的开关；新鲜生成）。</summary>
    public static string BuildOptionsOverlay(FamoSettings settings)
    {
        EngineSettings e = settings.Engine;
        SwitchSettings sw = settings.Switches;
        var sb = new System.Text.StringBuilder();
        sb.Append("# famo-options.yaml — 法墨即时开关（设置面板生成，请勿手改）。\n");
        sb.Append("# FamoRuntime 经 --control reload-options 重读并 set_option 到各会话，无需部署。\n");
        sb.Append("options:\n");
        sb.Append("  ascii_mode: ").Append(sw.AsciiMode ? "true" : "false").Append('\n');
        sb.Append("  ascii_punct: ").Append(sw.AsciiPunct ? "true" : "false").Append('\n');
        sb.Append("  full_shape: ").Append(sw.FullShape ? "true" : "false").Append('\n');
        // 简繁：rime-ice(全拼/双拼) 用 traditionalization；五笔用 zh_trad —— 同时发，繁体默认覆盖各方案。
        // set_option 到无此 option 的方案是无害空操作（对位 macOS 一次设多 option 名）。
        sb.Append("  traditionalization: ").Append(sw.Traditionalization ? "true" : "false").Append('\n');
        sb.Append("  zh_trad: ").Append(sw.Traditionalization ? "true" : "false").Append('\n');
        sb.Append("  emoji: ").Append(e.EmojiEnabled ? "true" : "false").Append('\n');
        return sb.ToString();
    }

    /// <summary>开关层落盘：把 famo-options.yaml 原子写到 famoDir，返回写入路径。</summary>
    public static string WriteOptionsOverlay(FamoSettings settings, string famoDir)
    {
        Directory.CreateDirectory(famoDir);
        string path = Path.Combine(famoDir, "famo-options.yaml");
        WriteAtomic(path, BuildOptionsOverlay(settings));
        return path;
    }

    // ─────────────────────────── 即时输入方式 (②) ───────────────────────────
    // famo-select-schema.txt 是单行 RIME schema id。native runtime 经独立 control 通道读入后对各会话
    // rime_api->select_schema(id)，并在新会话(AddSession)回放 ⇒ 输入方式跨 App/重启即时生效且持久，零部署。
    // 与部署桶的 schema_list(default.custom.yaml) 分层：本文件存在时 server 按它选方案（覆盖部署默认）。

    /// <summary>输入方式落盘：把当前 InputMethod 解析出的 schema id 原子写到 famo-select-schema.txt，返回写入路径。</summary>
    public static string WriteSelectSchema(FamoSettings settings, string famoDir)
    {
        Directory.CreateDirectory(famoDir);
        string path = Path.Combine(famoDir, "famo-select-schema.txt");
        WriteAtomic(path, settings.InputMethod.ResolveSchemaId() + "\n");
        return path;
    }

    /// <summary>明暗语义：system=亮/暗自动；light=强制亮；dark=强制暗。</summary>
    private static (string scheme, string schemeDark) ResolveColorSchemes(AppearanceSettings a)
    {
        string light = a.Skin;
        string dark = a.Skin + "_dark";
        return a.AppearanceMode switch
        {
            "light" => (light, light),
            "dark" => (dark, dark),
            _ => (light, dark), // system
        };
    }

    /// <summary>候选格式映射到 Weasel 已支持的标签/注释 style 键。</summary>
    private static (string labelFormat, string labelPoint, string commentPoint) ResolveCandidateFormat(AppearanceSettings a)
    {
        string labelPoint = Num(Math.Round(a.FontPoint * 0.72, 1, MidpointRounding.AwayFromZero));
        string commentPoint = Num(Math.Round(a.FontPoint * 0.8, 1, MidpointRounding.AwayFromZero));
        return a.CandidateFormat switch
        {
            "no_comment" => ("%s.", labelPoint, "0"),
            "candidate_only" => ("", "0", "0"),
            _ => ("%s.", labelPoint, commentPoint),
        };
    }

    /// <summary>
    /// 定点替换某个 style 标量键的值，保留原缩进与行尾注释。
    /// 这些键（color_scheme / font_point / horizontal / corner_radius …）在模板里各出现一次，
    /// 且不与 preset_color_schemes 的 *_color 键撞名，故全局替换安全。
    /// </summary>
    private static string SetScalar(string yaml, string key, string value)
    {
        // 匹配：缩进 + key: + 值（到行尾注释或行尾），保留缩进与 ` # 注释`。
        var rx = new Regex($@"(?m)^(?<indent>[ \t]*{Regex.Escape(key)}:[ \t]*)(?<val>[^\r\n#]*?)(?<trail>[ \t]*(#[^\r\n]*)?)$");
        Match m = rx.Match(yaml);
        if (!m.Success)
        {
            return yaml; // 模板无此键则跳过（容错）
        }
        return rx.Replace(yaml, me => me.Groups["indent"].Value + value + me.Groups["trail"].Value, 1);
    }

    // ─────────────────────────── 部署桶 ───────────────────────────
    // 改这些（schema_list / page_size / 模糊音 / emoji）需 FamoRuntime --control deploy 才生效。
    // 本切片只生成 YAML（buildable/testable）；实际 deploy 属 S4/S5 的 build-gated 部分。

    /// <summary>default.custom.yaml：整文件由 store 生成（覆盖 seed）。含 schema_list + page_size +
    /// 固定项（去 F4 switcher/hotkeys）+ 快捷键/键盘便利项（翻页/选词/符号/App英文，对齐 macOS）。
    /// 注意：必须包含固定项，否则保存部署设置会覆盖 seed 丢失去F4/右Shift（见 06-27-keyboard-convenience PRD）。</summary>
    public static string BuildDefaultCustom(FamoSettings settings)
    {
        ConvenienceSettings c = settings.Convenience;
        var sb = new System.Text.StringBuilder();
        sb.Append("# default.custom.yaml — 法墨 Windows (Famo)　由设置面板生成，请勿手改。\n");
        sb.Append("# 部署桶：方案菜单 + 候选字数 + 去F4 + 快捷键 + 键盘便利项。改后需重新部署生效。\n\n");
        sb.Append("patch:\n");

        // 方案菜单（只取 enabled，保序）+ 候选字数。
        sb.Append("  schema_list:\n");
        foreach (SchemaEntry e in settings.Engine.SchemaList)
        {
            if (e.Enabled)
            {
                sb.Append("    - schema: ").Append(e.Id).Append('\n');
            }
        }
        sb.Append("  menu/page_size: ").Append(settings.Engine.PageSize.ToString(CultureInfo.InvariantCulture)).Append('\n');

        // 固定项（必随每次生成，避免覆盖 seed 丢失）：
        //   去 F4/Ctrl+` 方案选单（switcher/hotkeys 置空，无方案概念，切换走 WinUI + select_schema）。
        sb.Append("  switcher/hotkeys: []\n");

        // 快捷键设置页：这些开关必须显式写出 on/off，不能靠底座默认值推断。
        sb.Append("  ascii_composer/good_old_caps_lock: ").Append(c.GoodOldCapsLock ? "true" : "false").Append('\n');
        string shift = c.ShiftSwitch ? "commit_code" : "noop";
        sb.Append("  ascii_composer/switch_key/Shift_L: ").Append(shift).Append('\n');
        sb.Append("  ascii_composer/switch_key/Shift_R: ").Append(shift).Append('\n');

        // 键盘便利项（对齐 macOS FamoRimePatchBuilder 的确定正确映射；逐项 gated，未开不发）。
        AppendConvenience(sb, c);
        return sb.ToString();
    }

    /// <summary>把便利项开关翻译成 key_binder/punctuator/app_options 的 patch 块（镜像 macOS 生成器）。</summary>
    private static void AppendConvenience(System.Text.StringBuilder sb, ConvenienceSettings c)
    {
        // [] 翻页：先停用 rime-ice 以词定字（select_character Lua 在 key_binder 前执行），
        // 置空这两个键后 [ ] 才落到下面的 key_binder 翻页绑定。
        if (c.PageBrackets)
        {
            sb.Append("  key_binder/select_first_character: \"\"\n");
            sb.Append("  key_binder/select_last_character: \"\"\n");
        }

        // 翻页 / 快速选词：生成法墨托管的 key_binder/bindings。
        // rime-ice 默认已启用 minus/equal；若用户关闭 -= 翻页，仅靠不追加无法生效，
        // 因此这里替换为一份保留上游常用基础热键的确定列表。
        var bindings = new List<string>();
        bindings.Add("    - { when: composing, accept: Shift+Tab, send: Shift+Left }");
        bindings.Add("    - { when: composing, accept: Tab, send: Shift+Right }");
        bindings.Add("    - { when: composing, accept: Alt+Left, send: Shift+Left }");
        bindings.Add("    - { when: composing, accept: Alt+Right, send: Shift+Right }");
        if (c.PageMinusEquals)
        {
            bindings.Add("    - { when: has_menu, accept: minus, send: Page_Up }");
            bindings.Add("    - { when: has_menu, accept: equal, send: Page_Down }");
        }
        if (c.PageBrackets)
        {
            bindings.Add("    - { when: has_menu, accept: bracketleft, send: Page_Up }");
            bindings.Add("    - { when: has_menu, accept: bracketright, send: Page_Down }");
        }
        if (c.PageCommaPeriod)
        {
            bindings.Add("    - { when: has_menu, accept: comma, send: Page_Up }");
            bindings.Add("    - { when: has_menu, accept: period, send: Page_Down }");
        }
        if (c.Select23Semicolon)
        {
            bindings.Add("    - { when: has_menu, accept: semicolon, send: 2 }");
            bindings.Add("    - { when: has_menu, accept: apostrophe, send: 3 }");
        }
        bindings.Add("    - { when: always, toggle: ascii_punct, accept: Control+Shift+3 }");
        bindings.Add("    - { when: always, toggle: ascii_punct, accept: Control+Shift+numbersign }");

        sb.Append("  key_binder/bindings:\n");
        foreach (string b in bindings) sb.Append(b).Append('\n');

        // 「/」→ 顿号：半角 + 全角标点表各追加一条（leaves '?' alone）。
        if (c.SlashToDun)
        {
            sb.Append("  \"punctuator/half_shape/+\":\n");
            sb.Append("    \"/\": { commit: \"、\" }\n");
            sb.Append("  \"punctuator/full_shape/+\":\n");
            sb.Append("    \"/\": { commit: \"、\" }\n");
        }

        // 数字间标点回退 ASCII（16:00、3.14、1,000）：librime 原生 digit_separators，非 Lua。
        if (c.DigitSeparators)
        {
            sb.Append("  punctuator/digit_separators: \",.:\"\n");
        }

        // App 默认英文：复用 Weasel app_options，按 exe 名各追加一条 ascii_mode: true。
        foreach (string exe in c.AppEnglishExes)
        {
            string name = exe.Trim();
            if (name.Length == 0) continue;
            sb.Append("  ").Append(Quote($"app_options/{name}/ascii_mode")).Append(": true\n");
        }
    }

    /// <summary>
    /// rime_ice.custom.yaml：以模板为基，注入 emoji reset，追加勾选的模糊音 speller/algebra/+。
    /// （tone_display 已随 oh-my-rime 一并下线，rime-ice 无此开关。）
    /// </summary>
    public static string BuildRimeIceCustom(FamoSettings settings, string? baseYaml = null)
    {
        string yaml = baseYaml ?? EmbeddedResources.RimeIceCustomTemplate;
        EngineSettings e = settings.Engine;

        // 仅改 emoji 块内的 reset（按 name 定位，避开其它 switch 的 reset）。
        yaml = SetSwitchReset(yaml, "emoji", e.EmojiEnabled ? 1 : 0);
        yaml = FuzzyBlockRx.Replace(yaml, "");
        yaml = LegacyFuzzyBlockRx.Replace(yaml, "");

        // 模糊音：9 独立对，各自 gated 追加 speller/algebra/+（锚定 derive，逐字段镜像
        // macOS FamoRimePatchBuilder.fuzzyAlgebraDerives；n/l、f/h、an/ang、en/eng、in/ing 双向发两条）。
        FuzzyPinyinSettings f = e.FuzzyPinyin;
        var rules = new List<(string rule, string note)>();
        if (f.ZhZ) rules.Add(("derive/^zh/z/", "zh → z"));
        if (f.ChC) rules.Add(("derive/^ch/c/", "ch → c"));
        if (f.ShS) rules.Add(("derive/^sh/s/", "sh → s"));
        if (f.NL) { rules.Add(("derive/^n/l/", "n → l")); rules.Add(("derive/^l/n/", "l → n")); }
        if (f.RL) rules.Add(("derive/^r/l/", "r → l"));
        if (f.FH) { rules.Add(("derive/^f/h/", "f → h")); rules.Add(("derive/^h/f/", "h → f")); }
        if (f.AnAng) { rules.Add(("derive/an$/ang/", "an → ang")); rules.Add(("derive/ang$/an/", "ang → an")); }
        if (f.EnEng) { rules.Add(("derive/en$/eng/", "en → eng")); rules.Add(("derive/eng$/en/", "eng → en")); }
        if (f.InIng) { rules.Add(("derive/in$/ing/", "in → ing")); rules.Add(("derive/ing$/in/", "ing → in")); }

        if (rules.Count > 0)
        {
            yaml = QuickSendBlockRx.Replace(yaml, "").TrimEnd('\r', '\n') + "\n";
            var sb = new System.Text.StringBuilder();
            sb.Append('\n').Append(FuzzyBegin).Append('\n');
            sb.Append("  \"speller/algebra/+\":\n");
            foreach ((string rule, string note) in rules)
                sb.Append("    - ").Append(rule).Append("  # ").Append(note).Append('\n');
            sb.Append(FuzzyEnd).Append('\n');
            yaml += sb.ToString();
        }
        return AppendQuickSendBlock(yaml);
    }

    // 法墨快捷短语 translator 注入块。拼音类用裸码精确触发；五笔仍走显式选择器插入。
    private const string QuickSendBegin = "  # >>> famo-quick-send >>>（设置面板生成，请勿手改）";
    private const string QuickSendEnd = "  # <<< famo-quick-send <<<";
    private static readonly Regex QuickSendBlockRx =
        new(@"[ \t]*#\s*>>> famo-quick-send >>>.*?#\s*<<< famo-quick-send <<<[^\n]*\r?\n?",
            RegexOptions.Singleline | RegexOptions.Compiled);

    private static string AppendQuickSendBlock(string yaml)
    {
        string merged = QuickSendBlockRx.Replace(yaml, "");
        if (!merged.Contains("patch:"))
            merged = (merged.Length == 0 ? "" : merged.TrimEnd('\r', '\n') + "\n") + "patch:\n";
        else if (!merged.EndsWith("\n"))
            merged += "\n";

        var block = new System.Text.StringBuilder();
        block.Append(QuickSendBegin).Append('\n');
        block.Append("  \"engine/translators/+\":\n");
        block.Append("    - table_translator@famo_quick_send\n");
        block.Append("  famo_quick_send:\n");
        block.Append("    dictionary: \"\"\n");
        block.Append("    user_dict: famo_quick_send\n");
        block.Append("    db_class: stabledb\n");
        block.Append("    enable_completion: false\n");
        block.Append("    enable_sentence: false\n");
        block.Append("    initial_quality: 99\n");
        block.Append(QuickSendEnd).Append('\n');
        return merged + block;
    }

    // 法墨五笔专属补丁块的起止标记。落盘时把本块整体替换/追加进既有 wubi86_jidian.custom.yaml，
    // 保留出厂（payload seed）已写入的 schema/icon 等品牌键，且重复部署不会重复追加 engine/filters/+。
    private const string WubiBegin = "  # >>> famo-wubi >>>（设置面板生成，请勿手改）";
    private const string WubiEnd = "  # <<< famo-wubi <<<";
    private static readonly Regex WubiBlockRx =
        new(@"[ \t]*#\s*>>> famo-wubi >>>.*?#\s*<<< famo-wubi <<<[^\n]*\r?\n?",
            RegexOptions.Singleline | RegexOptions.Compiled);

    /// <summary>
    /// wubi86_jidian.custom.yaml：五笔专属（部署桶），逐项镜像 macOS famoWubiCustomYAML。
    /// 无条件写（键只对五笔方案有效）。单字 lua 依赖随包 wubi86_jidian_single_char_*.lua。
    /// 不注入快捷短语 translator：五笔字母必须保留给五笔编码，快捷短语经显式选择器插入。
    /// <paramref name="baseYaml"/> 为既有文件内容时，保留其非本块内容（出厂 schema/icon 品牌键），
    /// 仅替换 famo 五笔块——对位 BuildRimeIceCustom 的 base 合并，避免部署冲掉五笔托盘图标。
    /// </summary>
    public static string BuildWubiCustom(FamoSettings settings, string? baseYaml = null)
    {
        WubiSettings w = settings.Engine.Wubi;

        // 本块（patch: 下 2 空格缩进的键），含起止标记，便于重复部署时整体替换。
        var block = new System.Text.StringBuilder();
        block.Append(WubiBegin).Append('\n');

        // 候选编码提示：on=保留原始编码(空 comment_format)，off=抹掉注释。
        if (w.CodeHint)
        {
            block.Append("  \"translator/comment_format\": []\n");
        }
        else
        {
            block.Append("  \"translator/comment_format\":\n");
            block.Append("    - xform/.+//\n");
        }

        // 空码自动清码：on=max_length，off=空串。
        block.Append("  \"speller/auto_clear\": ").Append(w.AutoClear ? "max_length" : "\"\"").Append('\n');

        // 单字候选：非 normal 才追 lua_filter（依赖随包 lua）。
        string? lua = w.CandidateMode switch
        {
            "single_first" => "lua_filter@*wubi86_jidian_single_char_first_filter",
            "single_only" => "lua_filter@*wubi86_jidian_single_char_only",
            _ => null,
        };
        if (lua != null)
        {
            block.Append("  \"engine/filters/+\":\n");
            block.Append("    - ").Append(lua).Append('\n');
        }

        // z 临时拼音反查：仅 OFF 时清空 recognizer 反查 pattern。
        if (!w.ZReverseLookup)
            block.Append("  \"recognizer/patterns/reverse_lookup\": \"\"\n");

        block.Append(WubiEnd).Append('\n');

        // 无 base（测试/极端缺文件）：独立成文。有 base：剥离旧 famo 块后把新块追加进同一 patch:，
        // 保留出厂 schema/icon 等键（与 rime_ice 合并同套路）。
        if (string.IsNullOrEmpty(baseYaml))
        {
            var sb = new System.Text.StringBuilder();
            sb.Append("# wubi86_jidian.custom.yaml — 法墨 Windows (Famo) 五笔专属，由设置面板生成，请勿手改。\n");
            sb.Append("patch:\n");
            sb.Append(block);
            return sb.ToString();
        }

        string merged = WubiBlockRx.Replace(baseYaml, "");
        if (!merged.Contains("patch:"))
            merged = (merged.Length == 0 ? "" : merged.TrimEnd('\r', '\n') + "\n") + "patch:\n";
        else if (!merged.EndsWith("\n"))
            merged += "\n";
        return merged + block;
    }

    /// <summary>部署桶落盘：写 default.custom.yaml + rime_ice.custom.yaml + wubi86_jidian.custom.yaml 到 famoDir。</summary>
    public static void WriteDeployBucket(FamoSettings settings, string famoDir)
    {
        Directory.CreateDirectory(famoDir);
        WriteAtomic(Path.Combine(famoDir, "default.custom.yaml"), BuildDefaultCustom(settings));

        string icePath = Path.Combine(famoDir, "rime_ice.custom.yaml");
        string? baseIce = File.Exists(icePath) ? File.ReadAllText(icePath) : null;
        WriteAtomic(icePath, BuildRimeIceCustom(settings, baseIce));

        string wubiPath = Path.Combine(famoDir, "wubi86_jidian.custom.yaml");
        string? baseWubi = File.Exists(wubiPath) ? File.ReadAllText(wubiPath) : null;
        WriteAtomic(wubiPath, BuildWubiCustom(settings, baseWubi));
    }

    private static void WriteAtomic(string path, string content) => SafeJsonFile.WriteAtomic(path, content);

    /// <summary>把 switches 列表里指定 name 那一条的 reset 设为 value（按 name 定位，块内首个 reset）。</summary>
    private static string SetSwitchReset(string yaml, string switchName, int value)
    {
        var rx = new Regex($@"(?s)(- name:\s*{Regex.Escape(switchName)}\b.*?reset:\s*)\d+");
        return rx.Replace(yaml, m => m.Groups[1].Value + value.ToString(CultureInfo.InvariantCulture), 1);
    }

    private static string Quote(string s) => "\"" + s.Replace("\"", "\\\"") + "\"";

    private static string Int(int v) => v.ToString(CultureInfo.InvariantCulture);

    private static string Num(double d) =>
        d == Math.Floor(d)
            ? ((long)d).ToString(CultureInfo.InvariantCulture)
            : d.ToString(CultureInfo.InvariantCulture);
}
