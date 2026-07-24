using System.Reflection;

namespace Famo.Settings.Core;

/// <summary>famo-config 的 default/schema JSON 以嵌入资源随程序集分发。</summary>
internal static class EmbeddedResources
{
    public static string DefaultSettingsJson => Read("famo-settings.default.json");
    public static string SchemaJson => Read("famo-settings.schema.json");
    public static string WeaselCustomTemplate => Read("weasel.custom.yaml");
    public static string RimeIceCustomTemplate => Read("rime_ice.custom.yaml");

    private static string Read(string logicalName)
    {
        Assembly asm = typeof(EmbeddedResources).Assembly;
        using Stream stream = asm.GetManifestResourceStream(logicalName)
            ?? throw new InvalidOperationException($"embedded resource not found: {logicalName}");
        using var reader = new StreamReader(stream);
        return reader.ReadToEnd();
    }
}
