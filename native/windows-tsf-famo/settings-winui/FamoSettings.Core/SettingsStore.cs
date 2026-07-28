using System.Text.Encodings.Web;
using System.Text.Json;

namespace Famo.Settings.Core;

/// <summary>
/// 设置 store 读写层。首次启动从内置 famo-settings.default.json seed 到
/// %LOCALAPPDATA%\Famo\famo-settings.json；之后读写遵 famo-settings.schema.json。
/// </summary>
public sealed class SettingsStore
{
    internal static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true,
        // 保持中文（字体名/皮肤名等）可读，不转义为 \uXXXX。
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    /// <summary>store 文件路径（默认 %LOCALAPPDATA%\Famo\famo-settings.json，测试可注入临时路径）。</summary>
    public string FilePath { get; }

    public SettingsStore(string? filePath = null)
    {
        FilePath = filePath ?? FamoPaths.SettingsFile;
    }

    /// <summary>
    /// 读取设置。文件不存在时从内置默认 seed 落盘后返回（首启路径）。
    /// </summary>
    public FamoSettings Load()
    {
        if (!File.Exists(FilePath))
        {
            FamoSettings seeded = CreateDefault();
            Save(seeded);
            return seeded;
        }

        // 解析失败时 SafeJsonFile.Read 会先把原文件备份成 .bak 再抛出，绝不在这里静默清空/覆盖
        // 用户的设置文件——是否回退默认、要不要落盘由调用方决定。
        bool migrated = false;
        FamoSettings loaded = SafeJsonFile.Read(FilePath, json =>
        {
            FamoSettings loaded = JsonSerializer.Deserialize<FamoSettings>(json, JsonOptions)
                ?? throw new InvalidDataException($"无法解析设置文件：{FilePath}");
            using JsonDocument doc = JsonDocument.Parse(json);
            EnsureMigrationInputs(loaded);
            MigrateLegacyFuzzy(doc.RootElement, loaded);
            migrated = MigrateSettingsVersion(doc.RootElement, loaded);
            ValidateOrThrow(loaded, FilePath);
            return loaded;
        });
        if (migrated) Save(loaded);
        return loaded;
    }

    private static bool MigrateSettingsVersion(JsonElement root, FamoSettings settings)
    {
        int oldVersion = root.TryGetProperty("version", out JsonElement version)
            && version.TryGetInt32(out int parsed)
            ? parsed
            : 1;

        bool changed = false;
        if (!root.TryGetProperty("engine", out JsonElement engine)
            || engine.ValueKind != JsonValueKind.Object
            || !engine.TryGetProperty("schemaList", out _))
        {
            settings.Engine.SchemaList = CreateDefault().Engine.SchemaList;
            changed = true;
        }
        if (oldVersion < 2 && settings.Appearance.InlinePreedit)
        {
            settings.Appearance.InlinePreedit = false;
            changed = true;
        }
        LayoutSettings layout = settings.Appearance.Layout;
        if (oldVersion < 3 && layout.CornerRadius == 8 && layout.BorderWidth == 1
            && layout.ShadowRadius == 4 && layout.Margin == 12)
        {
            layout.CornerRadius = 13;
            layout.ShadowRadius = 16;
            layout.Margin = 8;
            changed = true;
        }
        if (oldVersion < 4 && settings.Appearance.PreviewPages
            && settings.Appearance.Orientation != "vertical")
        {
            settings.Appearance.Orientation = "scroll";
            settings.Appearance.PreviewPages = false;
            changed = true;
        }
        if (oldVersion < 4 && root.TryGetProperty("ai", out JsonElement ai)
            && ai.ValueKind == JsonValueKind.Object
            && ai.TryGetProperty("documentFormattingSkillEnabled", out JsonElement legacyFormatting)
            && legacyFormatting.ValueKind is JsonValueKind.True or JsonValueKind.False)
        {
            settings.Ai.PublishFormattingSkillEnabled = legacyFormatting.GetBoolean();
            changed = true;
        }
        int pageSize = Math.Clamp(settings.Engine.PageSize, 3, 9);
        if (settings.Engine.PageSize != pageSize)
        {
            settings.Engine.PageSize = pageSize;
            changed = true;
        }
        if (settings.Version != FamoSettings.CurrentVersion)
        {
            settings.Version = FamoSettings.CurrentVersion;
            changed = true;
        }
        return changed;
    }

    private static void EnsureMigrationInputs(FamoSettings settings)
    {
        if (settings.Appearance is null || settings.Engine is null
            || settings.Appearance.Layout is null)
        {
            throw new InvalidDataException("设置文件的 appearance、engine 与 appearance.layout 不能为 null");
        }
    }

    /// <summary>
    /// 旧版 3 组合模糊音（zh_ch_sh / an_en_in / l_n_f_h_r_l）→ 新版 9 独立对的迁移。
    /// System.Text.Json 静默丢弃未知键，旧文件升级后这 9 项会全默认 false（丢设置）。
    /// 这里从原始 JSON 嗅探旧键：仅当出现旧键时按组合展开到对应新对（旧→新一对多）。
    /// 旧 zh_ch_sh→{ZhZ,ChC,ShS}、an_en_in→{AnAng,EnEng,InIng}、l_n_f_h_r_l→{NL,FH,RL}。
    /// </summary>
    private static void MigrateLegacyFuzzy(JsonElement root, FamoSettings settings)
    {
        if (!root.TryGetProperty("engine", out JsonElement engine)
            || engine.ValueKind != JsonValueKind.Object) return;
        if (!engine.TryGetProperty("fuzzyPinyin", out JsonElement fz)
            || fz.ValueKind != JsonValueKind.Object) return;

        FuzzyPinyinSettings f = settings.Engine.FuzzyPinyin
            ?? throw new InvalidDataException("设置文件的 engine.fuzzyPinyin 不能为 null");
        if (fz.TryGetProperty("zh_ch_sh", out JsonElement zcs) && zcs.ValueKind == JsonValueKind.True)
        { f.ZhZ = true; f.ChC = true; f.ShS = true; }
        if (fz.TryGetProperty("an_en_in", out JsonElement aei) && aei.ValueKind == JsonValueKind.True)
        { f.AnAng = true; f.EnEng = true; f.InIng = true; }
        if (fz.TryGetProperty("l_n_f_h_r_l", out JsonElement lnf) && lnf.ValueKind == JsonValueKind.True)
        { f.NL = true; f.FH = true; f.RL = true; }
    }

    /// <summary>写回设置（原子写：先写临时文件再替换，避免半写损坏）。</summary>
    public void Save(FamoSettings settings)
    {
        ValidateOrThrow(settings, FilePath);
        string dir = Path.GetDirectoryName(FilePath)!;
        Directory.CreateDirectory(dir);

        string json = JsonSerializer.Serialize(settings, JsonOptions);
        SafeJsonFile.WriteAtomic(FilePath, json);
    }

    private static void ValidateOrThrow(FamoSettings settings, string path)
    {
        SchemaValidationResult result = SchemaValidator.Validate(settings);
        if (!result.IsValid)
        {
            throw new InvalidDataException(
                $"设置文件不符合 schema：{path}{Environment.NewLine}{string.Join(Environment.NewLine, result.Errors)}");
        }
    }

    /// <summary>由内置 famo-settings.default.json 反序列化出一份默认设置。</summary>
    public static FamoSettings CreateDefault() =>
        JsonSerializer.Deserialize<FamoSettings>(EmbeddedResources.DefaultSettingsJson, JsonOptions)
            ?? throw new InvalidDataException("内置默认设置 JSON 解析失败");

    /// <summary>内置默认设置的原始 JSON 文本。</summary>
    public static string DefaultSettingsJson => EmbeddedResources.DefaultSettingsJson;
}
