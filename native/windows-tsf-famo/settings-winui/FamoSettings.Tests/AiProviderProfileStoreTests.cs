using Famo.Settings.Core.Ai;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class AiProviderProfileStoreTests : IDisposable
{
    private readonly string _dir;
    private readonly string _file;
    private readonly FakeSecretStore _secrets = new();

    public AiProviderProfileStoreTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "famo-ai-providers-" + Guid.NewGuid().ToString("N"));
        _file = Path.Combine(_dir, "ai-providers.json");
    }

    public void Dispose()
    {
        if (Directory.Exists(_dir)) Directory.Delete(_dir, recursive: true);
    }

    [Fact]
    public void AddProfile_StoresMetadataButKeepsApiKeyOutOfJson()
    {
        var service = new AiProviderProfileService(new AiProviderProfileStore(_file), _secrets);

        AiProviderProfile profile = service.AddProfile(new AiProviderProfileDraft
        {
            DisplayName = "DeepSeek",
            Endpoint = "https://api.deepseek.com/v1/chat/completions",
            Model = "deepseek-chat",
            ApiKey = "sk-test-secret",
            MakeDefault = true,
            MaxCandidates = 9,
        });

        string json = File.ReadAllText(_file);
        Assert.Contains("DeepSeek", json);
        Assert.Contains("https://api.deepseek.com/v1/chat/completions", json);
        Assert.Contains("deepseek-chat", json);
        Assert.DoesNotContain("sk-test-secret", json);
        Assert.Equal("sk-test-secret", _secrets.GetSecret(profile.SecretName));
        Assert.True(profile.IsDefault);
        Assert.Equal(5, profile.MaxCandidates);
        Assert.StartsWith("ai-provider:", profile.SecretName, StringComparison.Ordinal);
    }

    [Fact]
    public void AddProfile_MakesOnlyOneDefault()
    {
        var store = new AiProviderProfileStore(_file);
        var service = new AiProviderProfileService(store, _secrets);

        AiProviderProfile first = service.AddProfile(new AiProviderProfileDraft
        {
            DisplayName = "OpenAI",
            Endpoint = "https://api.openai.com/v1/chat/completions",
            Model = "gpt-4.1",
            ApiKey = "sk-first",
            MakeDefault = true,
        });
        AiProviderProfile second = service.AddProfile(new AiProviderProfileDraft
        {
            DisplayName = "DeepSeek",
            Endpoint = "https://api.deepseek.com/v1/chat/completions",
            Model = "deepseek-chat",
            ApiKey = "sk-second",
            MakeDefault = true,
        });

        IReadOnlyList<AiProviderProfile> profiles = store.Load();
        Assert.Equal(new[] { first.Id, second.Id }, profiles.Select(p => p.Id).ToArray());
        Assert.False(profiles[0].IsDefault);
        Assert.True(profiles[1].IsDefault);
    }

    [Fact]
    public void DeleteProfile_RemovesMetadataAndSecret()
    {
        var store = new AiProviderProfileStore(_file);
        var service = new AiProviderProfileService(store, _secrets);
        AiProviderProfile profile = service.AddProfile(new AiProviderProfileDraft
        {
            DisplayName = "OpenAI Compatible",
            Endpoint = "https://example.test/v1/chat/completions",
            Model = "legal-chat",
            ApiKey = "sk-delete-me",
            MakeDefault = true,
        });

        service.DeleteProfile(profile.Id);

        Assert.Empty(store.Load());
        Assert.Null(_secrets.GetSecret(profile.SecretName));
    }

    [Fact]
    public void Load_WhenJsonIsMalformed_ReturnsEmptyInsteadOfThrowing()
    {
        Directory.CreateDirectory(_dir);
        File.WriteAllText(_file, "{ not json");

        Assert.Empty(new AiProviderProfileStore(_file).Load());
    }

    [Fact]
    public void AddProfile_RejectsIncompleteProviderConfiguration()
    {
        var service = new AiProviderProfileService(new AiProviderProfileStore(_file), _secrets);

        InvalidDataException ex = Assert.Throws<InvalidDataException>(() =>
            service.AddProfile(new AiProviderProfileDraft
            {
                DisplayName = "",
                Endpoint = "http://insecure.example.test",
                Model = "",
                ApiKey = "",
            }));

        Assert.Contains("供应商名称", ex.Message);
    }

    [Fact]
    public void AddProfile_RejectsEmptyApiKey()
    {
        var service = new AiProviderProfileService(new AiProviderProfileStore(_file), _secrets);

        InvalidDataException ex = Assert.Throws<InvalidDataException>(() =>
            service.AddProfile(new AiProviderProfileDraft
            {
                DisplayName = "Fill In Later",
                Endpoint = "https://api.example.test/v1/chat/completions",
                Model = "",
                ApiKey = "",
                MakeDefault = true,
            }));

        Assert.Contains("API Key", ex.Message);
    }

    [Fact]
    public void AddProfile_AllowsEmptyModel_AndSurvivesReload()
    {
        var store = new AiProviderProfileStore(_file);
        var service = new AiProviderProfileService(store, _secrets);

        AiProviderProfile profile = service.AddProfile(new AiProviderProfileDraft
        {
            DisplayName = "Fill In Later",
            Endpoint = "https://api.example.test/v1/chat/completions",
            Model = "",
            ApiKey = "sk-test-secret",
            MakeDefault = true,
        });

        Assert.Equal(string.Empty, profile.Model);

        IReadOnlyList<AiProviderProfile> reloaded = store.Load();
        Assert.Single(reloaded);
        Assert.Equal(profile.Id, reloaded[0].Id);
        Assert.Equal(string.Empty, reloaded[0].Model);
    }

    private sealed class FakeSecretStore : ISecretStore
    {
        private readonly Dictionary<string, string> _values = new(StringComparer.Ordinal);

        public void SetSecret(string name, string value) => _values[name] = value;

        public string? GetSecret(string name) => _values.TryGetValue(name, out string? value) ? value : null;

        public void DeleteSecret(string name) => _values.Remove(name);
    }
}
