using System.Net;
using System.Text;
using Famo.Settings.Core;
using Famo.Settings.Core.Ai;
using Xunit;

namespace Famo.Settings.Tests;

/// <summary>
/// 润色护栏的线上契约。2026-07-20 实盘 A/B 证明这些红线有真实差异：无护栏版把
/// 「我觉得大概可能」压成一个「似乎」并抬成公文腔；带护栏版逐字保住 hedges、版本号与
/// 法条序号。断言护栏真的到了 HTTP 请求体——只断言常量字符串是假覆盖。
/// </summary>
public sealed class AiPolishGuardrailContractTests : IDisposable
{
    private readonly string _dir;
    private readonly string _file;
    private readonly FakeSecretStore _secrets = new();

    public AiPolishGuardrailContractTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "famo-ai-guardrail-" + Guid.NewGuid().ToString("N"));
        _file = Path.Combine(_dir, "ai-providers.json");
    }

    public void Dispose()
    {
        if (Directory.Exists(_dir)) Directory.Delete(_dir, recursive: true);
    }

    [Fact]
    public async Task PolishRequestBody_CarriesTheFaithfulnessGuardrails()
    {
        FamoSettings settings = SettingsStore.CreateDefault();
        settings.Ai.CloudEnabled = true;
        AddDefaultProfile();

        string captured = "";
        var http = new HttpClient(new CaptureHandler(request =>
        {
            captured = request.Content!.ReadAsStringAsync().GetAwaiter().GetResult();
            return new HttpResponseMessage(HttpStatusCode.OK)
            {
                Content = new StringContent(
                    """{ "choices": [ { "message": { "content": "{\"candidates\":[\"改写\"]}" } } ] }""",
                    Encoding.UTF8,
                    "application/json"),
            };
        }));

        var service = new AiSelectionSkillService(settings, new AiProviderProfileStore(_file), _secrets, http);
        await service.RunAsync(AiSelectionSkills.Polish, "我觉得大概可能有点问题。", CancellationToken.None);

        // 重改写角色 + 语种一致硬规则
        Assert.Contains("Heavy Rewriter", captured);
        Assert.Contains("Never translate", captured);
        // 忠实度红线：hedges 保持、不加寒暄/元话语
        Assert.Contains("Keep hedges as hedges", captured);
        Assert.Contains("meta-sentences", captured);
        // byte-for-byte 保留清单，含法墨的核心场景——法条引用
        Assert.Contains("byte-for-byte", captured);
        Assert.Contains("案号、法条序号、规范性文件名称", captured);
        // JSON 输出契约没被改坏（解析器靠它）
        Assert.Contains("candidates", captured);
    }

    private void AddDefaultProfile()
    {
        var service = new AiProviderProfileService(new AiProviderProfileStore(_file), _secrets);
        service.AddProfile(new AiProviderProfileDraft
        {
            DisplayName = "Probe",
            Endpoint = "https://api.deepseek.com/v1/chat/completions",
            Model = "gpt-4o",
            ApiKey = "sk-secret",
            MakeDefault = true,
        });
    }

    private sealed class CaptureHandler(Func<HttpRequestMessage, HttpResponseMessage> send)
        : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(
            HttpRequestMessage request, CancellationToken cancellationToken)
            => Task.FromResult(send(request));
    }

    private sealed class FakeSecretStore : ISecretStore
    {
        private readonly Dictionary<string, string> _values = new(StringComparer.Ordinal);

        public void SetSecret(string name, string value) => _values[name] = value;

        public string? GetSecret(string name) => _values.TryGetValue(name, out string? v) ? v : null;

        public void DeleteSecret(string name) => _values.Remove(name);
    }
}
