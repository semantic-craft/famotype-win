namespace Famo.Settings.Core.Emoji;

/// <summary>一个可选字形。<c>Wide</c>（颜文字）独占整行，其余在网格平铺。逐字段镜像 macOS FamoGlyph。</summary>
public readonly record struct FamoGlyph(string Char, string Keywords, bool Wide);

/// <summary>面板分类（顶部 tab）。Recent 由 EmojiRecentsStore 支撑。对位 macOS FamoEmojiCategory。</summary>
public enum FamoEmojiCategory
{
    Recent,
    Emoji,
    Symbol,
    Kaomoji,
    Punct,
}

/// <summary>分类的展示文案 / 图标。</summary>
public static class FamoEmojiCategoryInfo
{
    public static string Label(FamoEmojiCategory c) => c switch
    {
        FamoEmojiCategory.Recent => "最近",
        FamoEmojiCategory.Emoji => "表情",
        FamoEmojiCategory.Symbol => "符号",
        FamoEmojiCategory.Kaomoji => "颜文字",
        FamoEmojiCategory.Punct => "标点",
        _ => "",
    };

    public static string Glyph(FamoEmojiCategory c) => c switch
    {
        FamoEmojiCategory.Recent => "🕘",
        FamoEmojiCategory.Emoji => "😀",
        FamoEmojiCategory.Symbol => "§",
        FamoEmojiCategory.Kaomoji => "ツ",
        FamoEmojiCategory.Punct => "《",
        _ => "",
    };
}

/// <summary>
/// 表情/符号面板静态数据，逐字符移植 macOS FamoEmojiData（ADR-0001/0002/0003：纯数据，
/// 不碰 Rime / 热路径）。「最近」见 <see cref="EmojiRecentsStore"/>。
/// </summary>
public static class FamoEmojiData
{
    private static FamoGlyph G(string ch, string kw) => new(ch, kw, false);
    private static FamoGlyph W(string ch, string kw) => new(ch, kw, true);

    public static readonly IReadOnlyList<FamoGlyph> Emoji = new[]
    {
        G("😀", "smile 笑 开心"), G("😄", "笑 happy"), G("😁", "grin"), G("😂", "laugh 笑哭 哭笑"),
        G("🤣", "rofl 大笑"), G("😊", "smile 微笑"), G("🙂", "slight"), G("😉", "wink 眨眼"),
        G("😍", "love 爱 心 喜欢"), G("😘", "kiss 亲"), G("😎", "cool 酷"), G("🤩", "star 星星眼"),
        G("🥳", "party 庆祝"), G("😏", "smirk"), G("😴", "sleep 睡"), G("🤔", "think 思考"),
        G("😭", "cry 哭"), G("😅", "sweat"), G("😡", "angry 生气"), G("🥺", "plead 委屈"),
        G("👍", "thumbsup 赞 好 顶"), G("👎", "thumbsdown 踩"), G("👏", "clap 鼓掌"), G("🙏", "pray 拜托 谢谢"),
        G("💪", "muscle 加油"), G("🤝", "handshake 握手"), G("❤️", "heart 心 爱"), G("🧡", "orange heart"),
        G("💛", "yellow heart"), G("💚", "green heart"), G("💙", "blue heart"), G("💜", "purple heart"),
        G("🔥", "fire 火 热"), G("✨", "sparkle 闪"), G("🎉", "party 庆祝 撒花"), G("🎊", "confetti"),
        G("⭐", "star 星"), G("💯", "hundred 满分 百分"), G("✅", "check 对"), G("❌", "cross 错"),
        G("💡", "idea 灯泡"), G("🚀", "rocket 火箭"),
    };

    public static readonly IReadOnlyList<FamoGlyph> Symbol = new[]
    {
        G("×", "times 乘 叉"), G("÷", "divide 除"), G("±", "plusminus 正负"), G("∓", ""),
        G("°", "degree 度"), G("′", ""), G("″", ""), G("µ", "micro"),
        G("§", "section 节 章节"), G("¶", "pilcrow 段落"), G("→", "arrow 箭头 右"), G("←", "arrow 左"),
        G("↑", "上"), G("↓", "下"), G("↔", ""), G("⇒", "implies"),
        G("★", "star 星 实心"), G("☆", "star 星 空心"), G("♥", "heart 心"), G("♦", ""),
        G("♣", ""), G("♠", ""), G("✓", "check 对勾"), G("✗", "cross"),
        G("∞", "infinity 无穷"), G("≈", "approx 约"), G("≠", "neq 不等"), G("≤", "leq"),
        G("≥", "geq"), G("√", "root 根号"), G("∑", "sum 求和"), G("∫", "integral 积分"),
        G("π", "pi"), G("Ω", "omega"), G("©", "copyright 版权"), G("®", "registered"),
        G("™", "trademark 商标"), G("№", "number 编号"), G("‰", "permille 千分"), G("†", "dagger"),
    };

    public static readonly IReadOnlyList<FamoGlyph> Kaomoji = new[]
    {
        W("(๑•́ ₃ •̀๑)", "撇嘴 委屈"), W("¯\\_(ツ)_/¯", "耸肩 摊手 shrug"),
        W("(╯°□°)╯︵ ┻━┻", "掀桌 flip"), W("┬─┬ ノ( ゜-゜ノ)", "摆正 putback"),
        W("(´；ω；｀)", "哭 cry"), W("(≧▽≦)", "开心 happy"), W("(＾▽＾)", "笑 smile"),
        W("(╥﹏╥)", "大哭"), W("(ಠ_ಠ)", "无语 disapprove"), W("ʕ•ᴥ•ʔ", "熊 bear"),
        W("(☞ﾟヮﾟ)☞", "指 point"), W("(◍•ᴗ•◍)", "可爱 cute"), W("(｀・ω・´)", "认真"),
        W("(ノ°▽°)ノ︵", "撒"), W("σ(￣、￣〃)", "思考"), W("(*/ω＼*)", "害羞 shy"),
    };

    public static readonly IReadOnlyList<FamoGlyph> Punct = new[]
    {
        G("《》", "书名号 shumihao"), G("「」", "引号 quote"), G("『』", "引号 双书名"), G("【】", "方头括号"),
        G("（）", "括号 paren"), G("〈〉", "尖括号"), G("—", "破折号 dash"), G("…", "省略号 ellipsis"),
        G("§", "节 section"), G("¶", "段落 paragraph"), G("、", "顿号"), G("；", "分号 semicolon"),
        G("：", "冒号 colon"), G("·", "间隔号 middot"), G("※", "参考 reference"), G("～", "波浪 tilde"),
        G("‖", "双竖线"), G("＂", "引号"), G("•", "项目符号 bullet"), G("‹›", "单尖括号"),
        G("«»", "法文引号"), G("¡", "倒叹号"), G("¿", "倒问号"), G("℃", "摄氏度"),
    };

    /// <summary>固定（非「最近」）分类的条目。</summary>
    public static IReadOnlyList<FamoGlyph> ItemsFor(FamoEmojiCategory category) => category switch
    {
        FamoEmojiCategory.Recent => System.Array.Empty<FamoGlyph>(), // 由 EmojiRecentsStore 处理
        FamoEmojiCategory.Emoji => Emoji,
        FamoEmojiCategory.Symbol => Symbol,
        FamoEmojiCategory.Kaomoji => Kaomoji,
        FamoEmojiCategory.Punct => Punct,
        _ => System.Array.Empty<FamoGlyph>(),
    };

    /// <summary>跨全部分类按 char 或 keyword 搜索（大小写不敏感）。空查询返回空。对齐 macOS search。</summary>
    public static IReadOnlyList<FamoGlyph> Search(string query)
    {
        string q = (query ?? "").Trim().ToLowerInvariant();
        if (q.Length == 0) return System.Array.Empty<FamoGlyph>();

        var all = new List<FamoGlyph>(Emoji.Count + Symbol.Count + Punct.Count + Kaomoji.Count);
        all.AddRange(Emoji);
        all.AddRange(Symbol);
        all.AddRange(Punct);
        all.AddRange(Kaomoji);
        return all.FindAll(g =>
            g.Char.ToLowerInvariant().Contains(q) || g.Keywords.ToLowerInvariant().Contains(q));
    }
}
