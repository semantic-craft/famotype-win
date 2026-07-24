using System.Text.Json.Serialization;

namespace Famo.Settings.Core;

/// <summary>
/// 法墨 Windows 设置 store 的强类型模型，逐字段镜像 famo-settings.schema.json。
/// 落点 %LOCALAPPDATA%\Famo\famo-settings.json。camelCase 由 JsonNamingPolicy 处理；
/// 仅 fuzzyPinyin 的 snake_case 键需显式 JsonPropertyName。
/// </summary>
public sealed class FamoSettings
{
    public const int CurrentVersion = 3;

    public int Version { get; set; } = CurrentVersion;
    public AppearanceSettings Appearance { get; set; } = new();
    public EngineSettings Engine { get; set; } = new();
    public SwitchSettings Switches { get; set; } = new();
    public InputMethodSettings InputMethod { get; set; } = new();
    public ConvenienceSettings Convenience { get; set; } = new();
    public ClipboardSettings Clipboard { get; set; } = new();
    public AiSettings Ai { get; set; } = new();
}

/// <summary>AI 全局设置（AI 助手页 + 技能平台页共用）。云端 AI 默认关闭，只在用户主动触发的功能中读取。
/// 划词菜单总开关 + 4 个内置技能各自开关默认全部开启（受信任内置技能，无导入信任边界）。</summary>
public sealed class AiSettings
{
    public bool CloudEnabled { get; set; }

    /// <summary>划词菜单总开关：关闭后任意技能菜单项点击都提示「划词菜单已关闭」，不再逐个检查具体技能开关。</summary>
    public bool SelectionMenuEnabled { get; set; } = true;

    /// <summary>AI 润色选中技能开关。</summary>
    public bool PolishSkillEnabled { get; set; } = true;

    /// <summary>来源核验技能开关。</summary>
    public bool SourceCheckSkillEnabled { get; set; } = true;

    /// <summary>辅助检索技能开关。</summary>
    public bool ResearchAssistSkillEnabled { get; set; } = true;

    /// <summary>公文排版技能开关。</summary>
    public bool DocumentFormattingSkillEnabled { get; set; } = true;

    /// <summary>提示词优化技能开关。</summary>
    public bool PromptOptimizeSkillEnabled { get; set; } = true;
}

/// <summary>剪贴板历史（纯本地、默认关闭）。启用后仍只按用户点击捕获，不做后台监听。</summary>
public sealed class ClipboardSettings
{
    public bool Enabled { get; set; }
}

/// <summary>键盘便利项（部署桶）—— 翻页/选词/符号/App英文，对齐 macOS FamoRimePatchBuilder 的确定正确映射。
/// 全部翻译成 default.custom.yaml 的 key_binder/punctuator/app_options，改后需 /deploy 生效。
/// 成对标点、Command 选 2/3 不实现（用户拍板 / macOS 注释为需真机）。</summary>
public sealed class ConvenienceSettings
{
    /// <summary>左右 Shift 切换中英：Shift_L/Shift_R → commit_code；关闭时显式 noop。</summary>
    public bool ShiftSwitch { get; set; } = true;

    /// <summary>Caps Lock 保持大写锁语义：ascii_composer/good_old_caps_lock。</summary>
    public bool GoodOldCapsLock { get; set; } = true;

    /// <summary>- / = 翻页：minus/equal → Page_Up/Down；关闭时从托管 key_binder 列表移除。</summary>
    public bool PageMinusEquals { get; set; } = true;

    /// <summary>[] 翻页：停用 rime-ice 以词定字 + bracketleft/right → Page_Up/Down。</summary>
    public bool PageBrackets { get; set; }

    /// <summary>逗号句号翻页：comma/period → Page_Up/Down。</summary>
    public bool PageCommaPeriod { get; set; }

    /// <summary>分号引号选 2/3 位：semicolon → send 2、apostrophe → send 3。</summary>
    public bool Select23Semicolon { get; set; }

    /// <summary>「/」→ 顿号：punctuator/half_shape|full_shape/+ 的 "/" commit 「、」。</summary>
    public bool SlashToDun { get; set; }

    /// <summary>数字间标点回退 ASCII（16:00、3.14、1,000）：punctuator/digit_separators ",.:"。</summary>
    public bool DigitSeparators { get; set; }

    /// <summary>成对标点自动补全（搜狗式标点配对）：打左括号自动补右括号且光标居中，
    /// 紧接着打右括号则越过不重复。即时桶：famo-style.yaml 的 famo_auto_pair，TSF 客户端消费。</summary>
    public bool AutoPairPunctuation { get; set; }

    /// <summary>中英文之间自动加空格：commit 文本里中文与英文字母边界自动插入一个空格，
    /// 跨两次独立 commit 仍生效。即时桶：famo-style.yaml 的 famo_cjk_english_spacing，TSF 客户端消费。</summary>
    public bool CjkEnglishSpacing { get; set; }

    /// <summary>中文与数字之间自动加空格：commit 文本里中文与阿拉伯数字边界自动插入一个空格，
    /// 跨两次独立 commit 仍生效。即时桶：famo-style.yaml 的 famo_cjk_number_spacing，TSF 客户端消费。</summary>
    public bool CjkNumberSpacing { get; set; }

    /// <summary>这些 App 默认英文模式（按 exe 名，如 devenv.exe）：每条生成 app_options/&lt;exe&gt;/ascii_mode: true。</summary>
    public List<string> AppEnglishExes { get; set; } = new();
}

/// <summary>输入方式（搜狗式快切）—— 即时桶(select_schema)：写 famo-select-schema.txt +
/// FamoRuntime --control select-schema，运行中的 session 立刻切到目标 schema，零部署。
/// 与「键盘输入」页的部署式方案管理（schema_list 重排 + /deploy）分两层：本页主选当前输入方式，那页全量启用/排序。
/// AddSession 时 server 读本文件覆盖部署默认 ⇒ 重启后保持上次所选。</summary>
public sealed class InputMethodSettings
{
    /// <summary>输入方式：pinyin(全拼,默认) / double_pinyin(双拼) / wubi(五笔)。</summary>
    public string Method { get; set; } = "pinyin";

    /// <summary>双拼布局（仅 Method=double_pinyin 时生效）：
    /// flypy(小鹤,默认)/natural(自然码)/mspy(微软)/sogou(搜狗)/abc(智能ABC)/ziguang(紫光)/jiajia(拼音加加)。</summary>
    public string DoublePinyinLayout { get; set; } = "flypy";

    /// <summary>五笔方案（仅 Method=wubi 时生效）：jidian86(86 版五笔,默认)/pinyinMix(五笔拼音混输)。</summary>
    public string WubiScheme { get; set; } = "jidian86";

    /// <summary>把当前选择解析成 RIME schema id（= 写入 famo-select-schema.txt 的值）。
    /// 全部 id 均在 default.custom.yaml 的 schema_list 中（首启已编译 prism，秒切前提）。</summary>
    public string ResolveSchemaId() => Method switch
    {
        "wubi" => WubiScheme switch
        {
            "pinyinMix" => "wubi86_jidian_pinyin",
            _ => "wubi86_jidian",
        },
        "double_pinyin" => DoublePinyinLayout switch
        {
            "natural" => "double_pinyin",
            "mspy" => "double_pinyin_mspy",
            "sogou" => "double_pinyin_sogou",
            "abc" => "double_pinyin_abc",
            "ziguang" => "double_pinyin_ziguang",
            "jiajia" => "double_pinyin_jiajia",
            _ => "double_pinyin_flypy", // flypy（小鹤，默认）
        },
        _ => "rime_ice", // pinyin（全拼，默认）
    };
}

/// <summary>即时开关桶(①) —— 运行时 RIME option，写 famo-options.yaml + FamoRuntime --control reload-options，零部署。
/// 仅含「set_option 可切」的 switch（emoji 在 engine 内另有部署桶 reset 兜底，故不重复于此）。</summary>
public sealed class SwitchSettings
{
    /// <summary>中/英文输入：false=中文输入(默认)，true=英文直接输入。RIME option ascii_mode。
    /// 经 famo-options.yaml 在 AddSession 回放 → 每个新会话的「开机默认输入态」（对位 macOS 默认状态·中英文）。</summary>
    public bool AsciiMode { get; set; }

    /// <summary>中/英标点：false=中文标点(默认)，true=英文标点。RIME option ascii_punct。</summary>
    public bool AsciiPunct { get; set; }

    /// <summary>全/半角：false=半角(默认)，true=全角。RIME option full_shape。</summary>
    public bool FullShape { get; set; }

    /// <summary>简/繁：false=简体(默认)，true=繁体。RIME option：rime-ice 用 traditionalization、
    /// 五笔用 zh_trad —— 两者并发，故 繁体默认对全拼/双拼/五笔均生效（对位 macOS 简繁多 option 集）。</summary>
    public bool Traditionalization { get; set; }
}

/// <summary>即时桶(instant) —— 写 famo-style.yaml，候选窗轻量 reload。</summary>
public sealed class AppearanceSettings
{
    /// <summary>学院皮肤：shenda(荔园红,默认) / stanford / wuda / xiada。</summary>
    public string Skin { get; set; } = "shenda";

    /// <summary>明暗模式：system / light / dark。</summary>
    public string AppearanceMode { get; set; } = "system";

    public string FontFace { get; set; } = "微软雅黑";

    /// <summary>字号 pt，范围 11–22（出厂 19，对齐 Mac 1.3.3 中档）。</summary>
    public double FontPoint { get; set; } = 19;

    /// <summary>候选格式：full(标签+候选+注释) / no_comment(标签+候选) / candidate_only(仅候选)。</summary>
    public string CandidateFormat { get; set; } = "full";

    /// <summary>候选排列：horizontal / vertical。</summary>
    public string Orientation { get; set; } = "horizontal";

    public bool InlinePreedit { get; set; }

    /// <summary>内嵌候选预览：光标处显示当前候选词的实际文字，而非拼音本身；区别于 InlinePreedit
    /// （只控制预编辑显示的位置，不控制显示的内容）。映射 style/preedit_type：false=composition/true=preview。</summary>
    public bool InlineCandidatePreview { get; set; }

    public LayoutSettings Layout { get; set; } = new();
}

/// <summary>候选窗几何样式，全部即时桶 —— style/layout/*。</summary>
public sealed class LayoutSettings
{
    public int CornerRadius { get; set; } = 13;
    public int BorderWidth { get; set; } = 1;
    public int ShadowRadius { get; set; } = 16;
    public int Margin { get; set; } = 8;
}

/// <summary>部署桶(deploy) —— 写 RIME yaml 后调 FamoRuntime --control deploy。</summary>
public sealed class EngineSettings
{
    public List<SchemaEntry> SchemaList { get; set; } = new();

    /// <summary>一屏候选词数，范围 1–30（出厂 8）。</summary>
    public int PageSize { get; set; } = 8;

    public FuzzyPinyinSettings FuzzyPinyin { get; set; } = new();

    public bool EmojiEnabled { get; set; }

    /// <summary>五笔专属设置（仅 wubi86_jidian 方案生效；写 wubi86_jidian.custom.yaml，部署桶）。</summary>
    public WubiSettings Wubi { get; set; } = new();
}

/// <summary>
/// 五笔专属设置 —— 逐字段镜像 macOS FamoKeyboardPreferences 五笔项。写 wubi86_jidian.custom.yaml
/// （部署桶）；键只对五笔方案有效，故无须按当前方案 gate（对位 macOS 无条件写 wubi custom）。
/// </summary>
public sealed class WubiSettings
{
    /// <summary>候选编码提示：true=显示原始编码(comment_format:[])，false=抹掉注释(xform/.+//)。出厂关。</summary>
    public bool CodeHint { get; set; }

    /// <summary>空码自动清码：true=speller/auto_clear:max_length，false=""。出厂关。</summary>
    public bool AutoClear { get; set; }

    /// <summary>单字候选模式：normal(默认) / single_first(单字优先) / single_only(纯单字)。
    /// 非 normal 时向 engine/filters/+ 追 lua_filter（依赖随包 wubi86_jidian_single_char_*.lua）。</summary>
    public string CandidateMode { get; set; } = "normal";

    /// <summary>z 临时拼音反查：true(默认,保留 schema 反查) / false(清 recognizer/patterns/reverse_lookup)。</summary>
    public bool ZReverseLookup { get; set; } = true;
}

/// <summary>方案条目；数组顺序=优先级，首个 enabled=true 即默认方案。</summary>
public sealed class SchemaEntry
{
    public string Id { get; set; } = string.Empty;
    public bool Enabled { get; set; }
}

/// <summary>
/// 模糊音规则开关 —— 9 个独立对，逐字段镜像 macOS FamoFuzzyPreferences。
/// 各对单独 gated 生成 speller/algebra derive（部署桶）。snake_case 键需显式映射。
/// 旧 3 组合（zh_ch_sh / an_en_in / l_n_f_h_r_l）由 SettingsStore.Load 迁移到这 9 项。
/// </summary>
public sealed class FuzzyPinyinSettings
{
    // ── 翘舌：zh/ch/sh → z/c/s（各自独立）──
    /// <summary>zh = z。derive/^zh/z/</summary>
    [JsonPropertyName("zh_z")] public bool ZhZ { get; set; }

    /// <summary>ch = c。derive/^ch/c/</summary>
    [JsonPropertyName("ch_c")] public bool ChC { get; set; }

    /// <summary>sh = s。derive/^sh/s/</summary>
    [JsonPropertyName("sh_s")] public bool ShS { get; set; }

    // ── 声母：n/l、r/l、f/h ──
    /// <summary>n = l（双向）。derive/^n/l/ + derive/^l/n/</summary>
    [JsonPropertyName("n_l")] public bool NL { get; set; }

    /// <summary>r = l。derive/^r/l/</summary>
    [JsonPropertyName("r_l")] public bool RL { get; set; }

    /// <summary>f = h（双向）。derive/^f/h/ + derive/^h/f/</summary>
    [JsonPropertyName("f_h")] public bool FH { get; set; }

    // ── 韵母：an/ang、en/eng、in/ing（双向）──
    /// <summary>an = ang（双向）。derive/an$/ang/ + derive/ang$/an/</summary>
    [JsonPropertyName("an_ang")] public bool AnAng { get; set; }

    /// <summary>en = eng（双向）。derive/en$/eng/ + derive/eng$/en/</summary>
    [JsonPropertyName("en_eng")] public bool EnEng { get; set; }

    /// <summary>in = ing（双向）。derive/in$/ing/ + derive/ing$/in/</summary>
    [JsonPropertyName("in_ing")] public bool InIng { get; set; }

    /// <summary>已开启的对数（驱动 UI 摘要「已开启 N 项」）。</summary>
    [JsonIgnore]
    public int EnabledCount =>
        (ZhZ ? 1 : 0) + (ChC ? 1 : 0) + (ShS ? 1 : 0) + (NL ? 1 : 0) + (RL ? 1 : 0)
        + (FH ? 1 : 0) + (AnAng ? 1 : 0) + (EnEng ? 1 : 0) + (InIng ? 1 : 0);
}
