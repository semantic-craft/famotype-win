using System.Text.Json;
using Json.Schema;

namespace Famo.Settings.Core;

public sealed record SchemaValidationResult(bool IsValid, IReadOnlyList<string> Errors)
{
    public static readonly SchemaValidationResult Ok = new(true, Array.Empty<string>());
}

/// <summary>
/// 用 famo-settings.schema.json（draft 2020-12）校验设置 store。
/// 校验 required / enum / range / type；x-famo-* 自定义关键字被忽略。
/// </summary>
public static class SchemaValidator
{
    private static readonly JsonSchema Schema = JsonSchema.FromText(EmbeddedResources.SchemaJson);

    private static readonly EvaluationOptions Options = new()
    {
        OutputFormat = OutputFormat.List,
    };

    public static SchemaValidationResult Validate(string settingsJson)
    {
        JsonDocument doc;
        try
        {
            doc = JsonDocument.Parse(settingsJson);
        }
        catch (JsonException ex)
        {
            return new SchemaValidationResult(false, new[] { $"JSON 解析失败: {ex.Message}" });
        }

        using (doc)
        {
            EvaluationResults results = Schema.Evaluate(doc.RootElement, Options);
            if (results.IsValid)
            {
                return SchemaValidationResult.Ok;
            }

            var errors = new List<string>();
            Collect(results, errors);
            if (errors.Count == 0)
            {
                errors.Add("schema 校验失败（无详细信息）");
            }
            return new SchemaValidationResult(false, errors);
        }
    }

    public static SchemaValidationResult Validate(FamoSettings settings)
        => Validate(JsonSerializer.Serialize(settings, SettingsStore.JsonOptions));

    private static void Collect(EvaluationResults results, List<string> errors)
    {
        if (!results.IsValid && results.Errors is { Count: > 0 })
        {
            string loc = results.InstanceLocation.ToString();
            string where = loc.Length == 0 ? "(root)" : loc;
            foreach (KeyValuePair<string, string> error in results.Errors)
            {
                errors.Add($"{where}: {error.Value}");
            }
        }

        if (results.Details is { } details)
        {
            foreach (EvaluationResults child in details)
            {
                Collect(child, errors);
            }
        }
    }
}
