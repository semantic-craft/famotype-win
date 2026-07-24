using System.Text.Json;
using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

/// <summary>
/// S2.3：即时桶（appearance）改动只写 appearance，绝不触碰 engine（部署桶）。
/// </summary>
public class AppearanceInstantBucketTests : IDisposable
{
    private readonly string _dir;
    private readonly string _file;

    public AppearanceInstantBucketTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "famo-test-" + Guid.NewGuid().ToString("N"));
        _file = Path.Combine(_dir, "famo-settings.json");
    }

    public void Dispose()
    {
        if (Directory.Exists(_dir)) Directory.Delete(_dir, recursive: true);
    }

    private static readonly JsonSerializerOptions CompareOptions =
        new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };

    private static string EngineJson(FamoSettings s) =>
        JsonSerializer.Serialize(s.Engine, CompareOptions);

    [Fact]
    public void AppearanceEdit_DoesNotTouchEngineBucket()
    {
        var store = new SettingsStore(_file);
        FamoSettings before = store.Load();
        string engineBefore = EngineJson(before);

        // 模拟 UI 即时桶改动：皮肤/字号/横竖排/圆角。
        before.Appearance.Skin = "wuda";
        before.Appearance.FontPoint = 18;
        before.Appearance.Orientation = "vertical";
        before.Appearance.Layout.CornerRadius = 12;
        store.Save(before);

        FamoSettings after = new SettingsStore(_file).Load();

        // 即时桶生效：
        Assert.Equal("wuda", after.Appearance.Skin);
        Assert.Equal(18, after.Appearance.FontPoint);
        Assert.Equal("vertical", after.Appearance.Orientation);
        Assert.Equal(12, after.Appearance.Layout.CornerRadius);

        // 部署桶 engine 逐字节未变：
        Assert.Equal(engineBefore, EngineJson(after));

        // 改动后仍通过 schema。
        SchemaValidationResult v = SchemaValidator.Validate(after);
        Assert.True(v.IsValid, string.Join("\n", v.Errors));
    }
}
