using System.Text.Encodings.Web;
using System.Text.Json;

namespace Famo.Settings.Core.Ai;

public sealed record AiSelectionSkillDefinition(
    string Id,
    string PageId,
    string Title,
    string ResultTitle,
    string RunningMessage,
    string CopiedMessage,
    string SystemPrompt);

public sealed record AiSelectionSkillResult(
    IReadOnlyList<string> Candidates,
    string ProviderId,
    string Model,
    AiSelectionSkillDefinition Skill);

public sealed record AiSelectionPolishResult(IReadOnlyList<string> Candidates, string ProviderId, string Model);

public static class AiSelectionSkills
{
    // 护栏移植自 macOS v2-responsay-aligned 提示词（去掉 Windows 没有的屏幕上下文段）。
    // 2026-07-20 实盘 A/B 证明这些红线有真实差异：无护栏版把「我觉得大概可能」压成一个
    // 「似乎」并抬成公文腔；带护栏版逐字保住 hedges、版本号与法条序号。法墨的用户在写
    // 法律文书——法条引用被"顺手改写"是事故，不是润色。
    public static readonly AiSelectionSkillDefinition Polish = new(
        "polish",
        "ai-polish",
        "AI 润色选中",
        "候选",
        "正在润色选中文本...",
        "润色结果已复制。",
        "You are a same-language Heavy Rewriter (重改写) for a passage the user selected on screen. " +
        "The input is the user's own text. Improve how it reads while keeping it the user's: " +
        "reorder, merge, or split sentences for clarity and flow; fix grammar and awkward phrasing; " +
        "default to fluent written Chinese (书面语).\n" +
        "Same source language rule (hard): Chinese in → Chinese out, English in → English out. Never translate.\n" +
        "Faithfulness red lines:\n" +
        "- Do not add facts, numbers, links, names, or claims the user did not write; do not drop essential information.\n" +
        "- Do not change the user's stance, intent, or degree of certainty. Keep hedges as hedges " +
        "(「大概」「可能」「我觉得」stay) — never make the text sound more sure or more committed than the original.\n" +
        "- Do not add greetings, sign-offs, summaries, or meta-sentences (e.g. 「我整理如下」「以下是」).\n" +
        "Keep exactly as written (byte-for-byte): code identifiers, commands, file paths, config keys, " +
        "full version numbers, acronyms, proper nouns, brand names, emoji; " +
        "法律引用逐字保留：案号、法条序号、规范性文件名称。\n" +
        "Return a JSON object only, exactly shaped as {\"candidates\":[\"改写一\",\"改写二\"]}. " +
        "The candidates array holds plain rewrite strings; no other keys, no prose, no code fences. Do not explain.");

    public static readonly AiSelectionSkillDefinition SourceCheck = new(
        "source-check",
        "ai-source-check",
        "来源核验",
        "核验",
        "正在生成来源核验清单...",
        "来源核验结果已复制。",
        "You help a legal professional verify sources for selected text. " +
        "Use only the selected text; do not browse, do not claim a fact is verified, and do not add unsupported facts. " +
        "Return a JSON object only, exactly shaped as {\"candidates\":[\"核验清单\"]}. " +
        "Each candidate should list concrete claims to verify, primary-source types to look for, and precise search terms. " +
        "Do not explain outside JSON and do not include code fences.");

    public static readonly AiSelectionSkillDefinition ResearchAssist = new(
        "research-assist",
        "ai-research",
        "辅助检索",
        "检索方案",
        "正在生成辅助检索方案...",
        "辅助检索结果已复制。",
        "You help a legal professional turn selected text into a focused research plan. " +
        "Use only the selected text; produce useful follow-up questions, search queries, and source categories. " +
        "Return a JSON object only, exactly shaped as {\"candidates\":[\"检索方案\"]}. " +
        "Do not answer with unsupported facts, do not explain outside JSON, and do not include code fences.");

    public static readonly AiSelectionSkillDefinition DocumentFormatting = new(
        "document-formatting",
        "ai-document-formatting",
        "公文排版",
        "排版结果",
        "正在生成公文排版建议...",
        "公文排版结果已复制。",
        "You reformat selected Chinese text into standard official-document (公文) style: lead with the conclusion, " +
        "use formal official register, and follow standard document structure. " +
        "Use only the selected text; preserve meaning and do not add facts. " +
        "Return a JSON object only, exactly shaped as {\"candidates\":[\"排版结果\"]}. " +
        "Do not explain outside JSON and do not include code fences.");

    /// <summary>提示词优化。走两态契约（终稿 or 澄清问题），不是别家的
    /// <c>{"candidates":[…]}</c>——只能用 <see cref="AiSelectionSkillService.OptimizePromptAsync"/> 跑，
    /// 别拿 <see cref="AiSelectionSkillService.RunAsync"/> 喂它。</summary>
    public static readonly AiSelectionSkillDefinition PromptOptimize = new(
        "prompt-optimize",
        "ai-prompt-optimize",
        "提示词优化",
        "终稿",
        "正在优化提示词...",
        "优化后的提示词已复制。",
        PromptOptimizer.SystemPrompt);

    public static readonly IReadOnlyList<AiSelectionSkillDefinition> BuiltIn =
    [
        Polish,
        SourceCheck,
        ResearchAssist,
        DocumentFormatting,
        PromptOptimize,
    ];

    public static AiSelectionSkillDefinition? FromPageId(string? pageId)
    {
        if (string.IsNullOrWhiteSpace(pageId)) return null;

        string normalized = pageId.Trim().ToLowerInvariant();
        return BuiltIn.FirstOrDefault(skill =>
            string.Equals(skill.PageId, normalized, StringComparison.OrdinalIgnoreCase)
            || string.Equals(skill.Id, normalized, StringComparison.OrdinalIgnoreCase));
    }

    /// <summary>该内置技能当前是否开启（不含划词菜单总开关；总开关由调用方单独检查）。未知 id 视为开启，
    /// 容错风格对齐 <see cref="FromPageId"/>。</summary>
    public static bool IsEnabled(FamoSettings settings, string? skillId)
    {
        if (string.IsNullOrWhiteSpace(skillId)) return true;

        return skillId.Trim().ToLowerInvariant() switch
        {
            "polish" => settings.Ai.PolishSkillEnabled,
            "source-check" => settings.Ai.SourceCheckSkillEnabled,
            "research-assist" => settings.Ai.ResearchAssistSkillEnabled,
            "document-formatting" => settings.Ai.DocumentFormattingSkillEnabled,
            "prompt-optimize" => settings.Ai.PromptOptimizeSkillEnabled,
            _ => true,
        };
    }
}

public sealed class AiSelectionSkillService
{
    public const int MaxSelectionLength = 2000;

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    private readonly FamoSettings _settings;
    private readonly AiProviderChatCompletionClient _client;

    public AiSelectionSkillService(
        FamoSettings settings,
        AiProviderProfileStore profiles,
        ISecretStore secrets,
        HttpClient? http = null)
    {
        _settings = settings;
        _client = new AiProviderChatCompletionClient(profiles, secrets, http);
    }

    public async Task<AiSelectionSkillResult> RunAsync(
        AiSelectionSkillDefinition skill,
        string selection,
        CancellationToken cancellationToken)
    {
        EnsureRunnable(selection);

        string payload = JsonSerializer.Serialize(new { text = selection }, JsonOptions);
        AiProviderChatCompletionResult response = await _client.SendAsync(
            new[]
            {
                new AiProviderChatMessage("system", skill.SystemPrompt),
                new AiProviderChatMessage("user", payload),
            },
            cancellationToken,
            jsonObjectResponse: true);

        return new AiSelectionSkillResult(
            ParseCandidates(response.Text),
            response.ProviderId,
            response.Model,
            skill);
    }

    /// <summary>提示词优化。与 <see cref="RunAsync"/> 共用云端/选区守卫，但走两态契约：
    /// user 消息是防注入三明治包住的草稿原文（不是 <c>{"text":…}</c>），回复按终稿 / 澄清问题解析。
    ///
    /// 思考仍由 <c>AiProviderChatCompletionClient</c> 统一关掉——macOS 这条线特意保留了 qwen 思考，
    /// 但 2026-07-20 的 A/B 实盘（24.4s → 2.5s，四要素补齐断言逐条不变）证明 Windows 不必跟。</summary>
    public async Task<PromptOptimizeOutcome> OptimizePromptAsync(
        string draft,
        IReadOnlyList<PromptClarification> clarifications,
        CancellationToken cancellationToken)
    {
        EnsureRunnable(draft);

        var messages = new List<AiProviderChatMessage>
        {
            new("system", PromptOptimizer.SystemPrompt),
            new("user", PromptOptimizer.Sandwich(draft)),
        };
        if (clarifications.Count > 0)
        {
            messages.Add(new AiProviderChatMessage("user", PromptOptimizer.ClarificationMessage(clarifications)));
        }

        AiProviderChatCompletionResult response = await _client.SendAsync(
            messages,
            cancellationToken,
            jsonObjectResponse: true);

        return PromptOptimizer.Parse(response.Text) with { Model = response.Model };
    }

    /// <summary>上传前的准入：任何 keychain / 网络动作之前先过，绝不把空选区或超大选区送上云端。</summary>
    private void EnsureRunnable(string selection)
    {
        if (!_settings.Ai.CloudEnabled)
        {
            throw new InvalidOperationException("云端 AI 未启用。");
        }
        if (string.IsNullOrWhiteSpace(selection))
        {
            throw new InvalidOperationException("未选中文本。");
        }
        if (selection.Length > MaxSelectionLength)
        {
            throw new InvalidOperationException("选中文本过长，已取消执行技能。");
        }
    }

    private static IReadOnlyList<string> ParseCandidates(string json)
    {
        try
        {
            using JsonDocument doc = JsonDocument.Parse(json);
            if (!doc.RootElement.TryGetProperty("candidates", out JsonElement candidates)
                || candidates.ValueKind != JsonValueKind.Array)
            {
                throw new InvalidOperationException("AI 润色响应缺少 candidates。");
            }

            var values = new List<string>();
            foreach (JsonElement item in candidates.EnumerateArray())
            {
                if (item.ValueKind != JsonValueKind.String) continue;
                string? value = item.GetString();
                if (!string.IsNullOrWhiteSpace(value))
                {
                    values.Add(value.Trim());
                }
            }

            if (values.Count == 0)
            {
                throw new InvalidOperationException("AI 润色响应为空。");
            }

            return values;
        }
        catch (JsonException ex)
        {
            throw new InvalidOperationException("AI 润色响应格式无法解析。", ex);
        }
    }
}

public sealed class AiSelectionPolishService
{
    public const int MaxSelectionLength = AiSelectionSkillService.MaxSelectionLength;

    private readonly AiSelectionSkillService _service;

    public AiSelectionPolishService(
        FamoSettings settings,
        AiProviderProfileStore profiles,
        ISecretStore secrets,
        HttpClient? http = null)
    {
        _service = new AiSelectionSkillService(settings, profiles, secrets, http);
    }

    public async Task<AiSelectionPolishResult> PolishAsync(string selection, CancellationToken cancellationToken)
    {
        AiSelectionSkillResult result = await _service.RunAsync(
            AiSelectionSkills.Polish,
            selection,
            cancellationToken);
        return new AiSelectionPolishResult(result.Candidates, result.ProviderId, result.Model);
    }
}
