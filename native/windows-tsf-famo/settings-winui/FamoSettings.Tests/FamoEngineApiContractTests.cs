using System.Text.RegularExpressions;
using Xunit;

namespace Famo.Settings.Tests;

public sealed class FamoEngineApiContractTests
{
    private const string HeaderPath = "native/windows-tsf-famo/engine-api/famo_engine_api.h";

    [Fact]
    public void Header_ExposesVersionedCAbiEntryPoint()
    {
        string header = Header();

        Assert.Contains("#define FAMO_ENGINE_ABI_VERSION 1u", header);
        Assert.Contains("#ifdef __cplusplus", header);
        Assert.Contains("extern \"C\"", header);
        Assert.Contains("FAMO_ENGINE_EXPORT", header);
        Assert.Contains("FAMO_ENGINE_CALL", header);
        Assert.Contains("FamoCreateEngineApi", header);
        Assert.Contains("FamoEngineApi* out_api", header);
    }

    [Fact]
    public void Header_PublicStructsAreSizePrefixedAndUtf8Owned()
    {
        string header = Header();
        var structs = Regex.Matches(
            header,
            @"typedef struct\s+(Famo\w+)\s*\{\s*(.*?)\s*\}\s+\1;",
            RegexOptions.Singleline);

        Assert.NotEmpty(structs);
        foreach (Match match in structs)
        {
            string name = match.Groups[1].Value;
            string body = match.Groups[2].Value.TrimStart();
            string expectedPrefix = name.EndsWith("V2", StringComparison.Ordinal)
                ? "uint32_t struct_size;"
                : "uint32_t size;";
            Assert.StartsWith(expectedPrefix, body);
        }

        Assert.Contains("typedef struct FamoUtf8String", header);
        Assert.Contains("const char* data;", header);
        Assert.Contains("uint32_t length_bytes;", header);
        Assert.Contains("void* (FAMO_ENGINE_CALL *alloc)(size_t bytes);", header);
        Assert.Contains("void (FAMO_ENGINE_CALL *free)(void* p);", header);
        Assert.Contains("free_view", header);
    }

    [Fact]
    public void Header_V2UsesAnIndependentOwnedActionResultAndExplicitLayout()
    {
        string header = Header();

        Assert.Contains("#define FAMO_ENGINE_ABI_V2 2u", header);
        Assert.Contains("FamoCreateEngineApiV2", header);
        Assert.Contains("typedef struct FamoEngineApiV2", header);
        Assert.Contains("FamoEngineActionRequestV2", header);
        Assert.Contains("FamoEngineActionResultV2** out_result", header);
        Assert.Contains("FAMO_COMPOSITION_LAYOUT_V2", header);
        Assert.Contains("FAMO_CANDIDATE_LAYOUT_V2", header);
        Assert.Contains("FAMO_CANDIDATE_V2_STRIDE", header);
        Assert.Contains("uint32_t candidate_stride;", header);
        Assert.Contains("uint32_t handled;", header);
        Assert.Contains("free_result", header);
        Assert.Contains(
            "int32_t (FAMO_ENGINE_CALL *deploy_schema)(\n      const FamoUtf8String* schema_id);",
            header);
    }

    [Fact]
    public void Header_DoesNotExposeLegacyOrPlatformRuntimeTypes()
    {
        string header = Header();
        string[] forbidden =
        {
            "Weasel",
            "ITf",
            "IUnknown",
            "HRESULT",
            "HWND",
            "BSTR",
            "WCHAR",
            "wchar_t",
            "CString",
            "std::",
            "std::string",
            "std::vector",
            "#include <windows.h>",
        };

        foreach (string token in forbidden)
        {
            Assert.DoesNotContain(token, header, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void Header_CoversGuideSurface()
    {
        string header = Header();
        string[] required =
        {
            "FAMO_ENGINE_E_RUNTIME",
            "FAMO_ENGINE_E_SCHEMA",
            "FAMO_ENGINE_E_DICT",
            "FAMO_ENGINE_E_USERDB",
            "FAMO_ENGINE_E_IPC",
            "FAMO_ENGINE_CAP_LUA",
            "FAMO_ENGINE_CAP_OPENCC",
            "FAMO_ENGINE_CAP_USERDB_SYNC",
            "FAMO_ENGINE_CAP_SCHEMA_DEPLOY",
            "FamoKeyEvent",
            "FamoCandidate",
            "FamoCompositionView",
            "get_info",
            "initialize",
            "shutdown",
            "create_context",
            "destroy_context",
            "process_key",
            "select_candidate",
            "set_option",
            "deploy_schema",
            "free_view",
        };

        foreach (string token in required)
        {
            Assert.Contains(token, header);
        }
    }

    private static string Header()
    {
        return File.ReadAllText(RepoFile(HeaderPath));
    }

    private static string RepoFile(string relativePath)
    {
        string? dir = AppContext.BaseDirectory;
        while (!string.IsNullOrEmpty(dir))
        {
            string candidate = Path.Combine(dir, relativePath.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate))
            {
                return candidate;
            }

            dir = Directory.GetParent(dir)?.FullName;
        }

        throw new FileNotFoundException($"Could not locate {relativePath}");
    }
}
