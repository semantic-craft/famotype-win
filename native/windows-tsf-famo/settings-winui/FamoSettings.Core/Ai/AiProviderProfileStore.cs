using System.Text.Encodings.Web;
using System.Text.Json;

namespace Famo.Settings.Core.Ai;

public static class AiProviderKinds
{
    public const string OpenAiCompatible = "openai-compatible";
}

public sealed class AiProviderProfile
{
    public string Id { get; set; } = string.Empty;
    public string DisplayName { get; set; } = string.Empty;
    public string Kind { get; set; } = AiProviderKinds.OpenAiCompatible;
    public string Endpoint { get; set; } = string.Empty;
    public string Model { get; set; } = string.Empty;
    public int MaxCandidates { get; set; } = 3;
    public string SecretName { get; set; } = string.Empty;
    public bool IsDefault { get; set; }
    public DateTimeOffset CreatedAt { get; set; }
    public DateTimeOffset UpdatedAt { get; set; }
}

public sealed class AiProviderProfileDraft
{
    public string DisplayName { get; set; } = string.Empty;
    public string Endpoint { get; set; } = string.Empty;
    public string Model { get; set; } = string.Empty;
    public string ApiKey { get; set; } = string.Empty;
    public bool MakeDefault { get; set; }
    public int MaxCandidates { get; set; } = 3;
}

public interface ISecretStore
{
    void SetSecret(string name, string value);
    string? GetSecret(string name);
    void DeleteSecret(string name);
}

public sealed class AiProviderProfileStore
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    public string FilePath { get; }

    public AiProviderProfileStore(string? filePath = null)
    {
        FilePath = filePath ?? FamoPaths.AiProviderProfilesFile;
    }

    /// <summary>供纯展示用：读取/反序列化失败时返回空列表，不抛出。</summary>
    public IReadOnlyList<AiProviderProfile> Load()
    {
        try
        {
            return LoadOrThrow();
        }
        catch
        {
            return Array.Empty<AiProviderProfile>();
        }
    }

    /// <summary>与 <see cref="Load"/> 相同，但读取/反序列化失败时直接抛出，不当作空列表处理，
    /// 避免 AddProfile/DeleteProfile/SetDefault 的读-改-写用空列表覆盖磁盘上已有的供应商配置。</summary>
    internal IReadOnlyList<AiProviderProfile> LoadOrThrow()
    {
        if (!File.Exists(FilePath)) return Array.Empty<AiProviderProfile>();

        return SafeJsonFile.Read(FilePath, json =>
        {
            AiProviderProfileFile? file = JsonSerializer.Deserialize<AiProviderProfileFile>(json, JsonOptions);
            return (IReadOnlyList<AiProviderProfile>)Normalize(file?.Profiles ?? new List<AiProviderProfile>()).ToArray();
        });
    }

    public AiProviderProfile? DefaultProfile() =>
        Load().FirstOrDefault(p => p.IsDefault);

    internal void Save(IEnumerable<AiProviderProfile> profiles)
    {
        string dir = Path.GetDirectoryName(FilePath)!;
        Directory.CreateDirectory(dir);

        var file = new AiProviderProfileFile
        {
            Profiles = Normalize(profiles).ToList(),
        };

        SafeJsonFile.WriteAtomic(FilePath, JsonSerializer.Serialize(file, JsonOptions));
    }

    internal static int ClampMaxCandidates(int value) => Math.Clamp(value, 1, 5);

    private static IEnumerable<AiProviderProfile> Normalize(IEnumerable<AiProviderProfile> profiles)
    {
        bool defaultSeen = false;
        foreach (AiProviderProfile profile in profiles.Where(IsPersistable))
        {
            profile.Kind = string.IsNullOrWhiteSpace(profile.Kind)
                ? AiProviderKinds.OpenAiCompatible
                : profile.Kind.Trim();
            profile.DisplayName = profile.DisplayName.Trim();
            profile.Endpoint = profile.Endpoint.Trim();
            profile.Model = profile.Model.Trim();
            profile.MaxCandidates = ClampMaxCandidates(profile.MaxCandidates);

            if (profile.IsDefault)
            {
                if (defaultSeen)
                {
                    profile.IsDefault = false;
                }
                else
                {
                    defaultSeen = true;
                }
            }

            yield return profile;
        }
    }

    private static bool IsPersistable(AiProviderProfile profile) =>
        !string.IsNullOrWhiteSpace(profile.Id)
        && !string.IsNullOrWhiteSpace(profile.DisplayName)
        && !string.IsNullOrWhiteSpace(profile.Endpoint)
        && !string.IsNullOrWhiteSpace(profile.SecretName);

    private sealed class AiProviderProfileFile
    {
        public int Version { get; set; } = 1;
        public List<AiProviderProfile> Profiles { get; set; } = new();
    }
}

public sealed class AiProviderProfileService
{
    private readonly AiProviderProfileStore _store;
    private readonly ISecretStore _secrets;

    public AiProviderProfileService(AiProviderProfileStore store, ISecretStore secrets)
    {
        _store = store;
        _secrets = secrets;
    }

    public AiProviderProfile AddProfile(AiProviderProfileDraft draft, DateTimeOffset? now = null)
    {
        ValidateDraft(draft);

        var profiles = _store.LoadOrThrow().ToList();
        string id = Guid.NewGuid().ToString("N");
        DateTimeOffset timestamp = now ?? DateTimeOffset.Now;
        bool isDefault = draft.MakeDefault || profiles.Count == 0;

        if (isDefault)
        {
            foreach (AiProviderProfile existing in profiles)
            {
                existing.IsDefault = false;
            }
        }

        var profile = new AiProviderProfile
        {
            Id = id,
            DisplayName = draft.DisplayName.Trim(),
            Endpoint = draft.Endpoint.Trim(),
            Model = draft.Model.Trim(),
            MaxCandidates = AiProviderProfileStore.ClampMaxCandidates(draft.MaxCandidates),
            SecretName = $"ai-provider:{id}",
            IsDefault = isDefault,
            CreatedAt = timestamp,
            UpdatedAt = timestamp,
        };

        _secrets.SetSecret(profile.SecretName, draft.ApiKey);
        profiles.Add(profile);
        _store.Save(profiles);
        return profile;
    }

    public bool DeleteProfile(string id)
    {
        var profiles = _store.LoadOrThrow().ToList();
        AiProviderProfile? deleted = profiles.FirstOrDefault(p => p.Id == id);
        if (deleted is null) return false;

        profiles.Remove(deleted);
        if (deleted.IsDefault && profiles.Count > 0 && profiles.All(p => !p.IsDefault))
        {
            profiles[0].IsDefault = true;
            profiles[0].UpdatedAt = DateTimeOffset.Now;
        }

        _store.Save(profiles);
        _secrets.DeleteSecret(deleted.SecretName);
        return true;
    }

    public AiProviderProfile SetDefault(string id)
    {
        var profiles = _store.LoadOrThrow().ToList();
        AiProviderProfile? selected = profiles.FirstOrDefault(p => p.Id == id);
        if (selected is null) throw new InvalidDataException("找不到要设为默认的 AI 供应商");

        foreach (AiProviderProfile profile in profiles)
        {
            profile.IsDefault = profile.Id == id;
            if (profile.IsDefault)
            {
                profile.UpdatedAt = DateTimeOffset.Now;
            }
        }

        _store.Save(profiles);
        return selected;
    }

    private static void ValidateDraft(AiProviderProfileDraft draft)
    {
        var errors = new List<string>();
        if (string.IsNullOrWhiteSpace(draft.DisplayName)) errors.Add("供应商名称不能为空");
        if (!IsAllowedEndpoint(draft.Endpoint)) errors.Add("Endpoint 必须是 HTTPS 地址，或本机 HTTP 地址");
        if (string.IsNullOrWhiteSpace(draft.ApiKey)) errors.Add("API Key 不能为空");

        if (errors.Count > 0)
        {
            throw new InvalidDataException(string.Join("；", errors));
        }
    }

    private static bool IsAllowedEndpoint(string endpoint)
    {
        if (!Uri.TryCreate(endpoint.Trim(), UriKind.Absolute, out Uri? uri)) return false;
        if (uri.Scheme == Uri.UriSchemeHttps) return true;
        if (uri.Scheme != Uri.UriSchemeHttp) return false;
        return uri.IsLoopback
            || string.Equals(uri.Host, "localhost", StringComparison.OrdinalIgnoreCase);
    }
}
