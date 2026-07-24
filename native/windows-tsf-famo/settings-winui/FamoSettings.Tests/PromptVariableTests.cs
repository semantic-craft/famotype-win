using Famo.Settings.Core.Prompts;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class PromptVariableTests
{
    [Fact]
    public void Extract_ReturnsUniqueVariablesInFirstSeenOrder()
    {
        IReadOnlyList<PromptVariable> variables = PromptVariableParser.Extract(
            "请基于{{ 案由 }}、{{法院}}和{{案由}}生成摘要。");

        Assert.Equal(["案由", "法院"], variables.Select(v => v.Name).ToArray());
        Assert.Equal("{{ 案由 }}", variables[0].Placeholder);
    }

    [Fact]
    public void Extract_IgnoresEmptyOrNestedBracesAndSupportsDefaults()
    {
        IReadOnlyList<PromptVariable> variables = PromptVariableParser.Extract(
            "{{ }} {{outer {{inner}} }} {{语言:中文}} {{期限: 7天 }}");

        Assert.Equal(["inner", "语言", "期限"], variables.Select(v => v.Name).ToArray());
        Assert.Null(variables[0].DefaultValue);
        Assert.Equal("中文", variables[1].DefaultValue);
        Assert.Equal("7天", variables[2].DefaultValue);
    }

    [Fact]
    public void Render_ReplacesProvidedValuesAndPreservesUnresolvedPlaceholders()
    {
        string rendered = PromptRenderer.Render(
            "案件：{{案由}}\n法院：{{法院}}\n语言：{{语言:中文}}",
            new Dictionary<string, string>
            {
                ["案由"] = "买卖合同纠纷",
                ["语言"] = "英文",
            });

        Assert.Equal("案件：买卖合同纠纷\n法院：{{法院}}\n语言：英文", rendered);
    }

    [Fact]
    public void Render_HandlesEmptyContent()
    {
        Assert.Equal("", PromptRenderer.Render(null, new Dictionary<string, string>()));
        Assert.Equal("", PromptRenderer.Render("", new Dictionary<string, string>()));
    }
}
