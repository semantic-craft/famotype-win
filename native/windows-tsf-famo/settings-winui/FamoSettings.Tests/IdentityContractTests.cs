using System.Text.Json;
using Xunit;

namespace Famo.Settings.Tests;

/// <summary>
/// apply-famo-identity.ps1 替换表完整性契约（并存承诺：法墨与日用小狼毫零共享命名对象）。
/// 背景：WeaselDeployerExclusiveMutex 曾漏出替换表，法墨 deployer/TSF 与日用 Weasel 共享互斥体
/// （07-02-ime-stability-standards P1-A）。本测试锁死映射 + 替换顺序 + 文件清单。
/// </summary>
public sealed class IdentityContractTests
{
    private const string IdentityScript = "native/windows-tsf-famo/weasel-fork/apply-famo-identity.ps1";
    private const string IdentityJson = "native/windows-tsf-famo/weasel-fork/famo-identity.json";
    private const string NativeGuidHeader = "native/windows-tsf-famo/text-service/src/famo_guids.h";
    private const string GlobalsCpp = "native/windows-tsf-famo/weasel-fork/overlay/WeaselTSF/Globals.cpp";
    private const string EnsureDeployedPatch = "native/windows-tsf-famo/weasel-fork/features/ensure-deployed.patch";
    private const string RuntimeIdentityHeader = "native/windows-tsf-famo/runtime-protocol/include/famo_runtime_identity.h";

    [Fact]
    public void StableNativeTextServiceClsid_MatchesIdentityJson()
    {
        // 稳定版原生 TSF 直接持有产品 CLSID；安装器只调用同一次构建产出的
        // FamoProfileTool 注册/反注册，不再手抄 GUID 或直接拼 CTF\TIP 注册表路径。
        string header = File.ReadAllText(RepoFile(NativeGuidHeader)).Replace("\r\n", "\n");

        Assert.Contains(
            WithoutWhitespace(ExpectedGuidLiteral(
                "inline constexpr GUID kTextServiceClsid",
                ReadIdentityGuid("clsidTextService"))),
            WithoutWhitespace(header));
    }

    [Fact]
    public void GlobalsCpp_TextServiceClsidAndProfileGuid_MatchIdentityJson()
    {
        // WeaselTSF/Globals.cpp 由 apply-famo-identity.ps1 拷贝覆盖到上游 checkout 并编译进
        // weaselx64.dll，是运行时真正注册的 CLSID/Profile GUID；c_clsidTextService/c_guidProfile
        // 是手抄字节数组，与 famo-identity.json 不同步会导致运行时 TSF 注册与真相源不一致。
        string cpp = File.ReadAllText(RepoFile(GlobalsCpp)).Replace("\r\n", "\n");

        Assert.Contains(
            ExpectedGuidLiteral("static const GUID c_clsidTextService", ReadIdentityGuid("clsidTextService")),
            cpp);
        Assert.Contains(
            ExpectedGuidLiteral("static const GUID c_guidProfile", ReadIdentityGuid("guidProfile")),
            cpp);
    }

    [Fact]
    public void GlobalsCpp_LangBarDisplayAttributeAndPreservedKeyGuids_MatchIdentityJson()
    {
        // 同一文件内的另外 3 个法墨专属 GUID（langbar 按钮/显示属性/模式切换热键），
        // 与上面的 clsid/profile 一样是手抄字节数组，同样需要锁死防漂移。
        string cpp = File.ReadAllText(RepoFile(GlobalsCpp)).Replace("\r\n", "\n");

        Assert.Contains(
            ExpectedGuidLiteral("static const GUID c_guidLangBarItemButton", ReadIdentityGuid("guidLangBarItemButton")),
            cpp);
        Assert.Contains(
            ExpectedGuidLiteral("static const GUID c_guidDisplayAttributeInput", ReadIdentityGuid("guidDisplayAttributeInput")),
            cpp);
        Assert.Contains(
            ExpectedGuidLiteral("const GUID GUID_IME_MODE_PRESERVED_KEY", ReadIdentityGuid("guidImeModePreservedKey")),
            cpp);
    }

    private static string ReadIdentityGuid(string propertyName)
    {
        using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(RepoFile(IdentityJson)));
        return doc.RootElement.GetProperty("guids").GetProperty(propertyName).GetString()
            ?? throw new InvalidOperationException($"famo-identity.json missing guids.{propertyName}");
    }

    private static string ReadIdentityBrand(string propertyName)
    {
        using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(RepoFile(IdentityJson)));
        return doc.RootElement.GetProperty("brand").GetProperty(propertyName).GetString()
            ?? throw new InvalidOperationException($"famo-identity.json missing brand.{propertyName}");
    }

    private static string ReadIdentityRegistry(string propertyName)
    {
        using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(RepoFile(IdentityJson)));
        return doc.RootElement.GetProperty("registry").GetProperty(propertyName).GetString()
            ?? throw new InvalidOperationException($"famo-identity.json missing registry.{propertyName}");
    }

    private static string ReadIdentityIpc(string propertyName)
    {
        using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(RepoFile(IdentityJson)));
        return doc.RootElement.GetProperty("ipc").GetProperty(propertyName).GetString()
            ?? throw new InvalidOperationException($"famo-identity.json missing ipc.{propertyName}");
    }

    [Fact]
    public void NativeRuntimeEndpointAndSingletonNames_MatchIdentityTruthSource()
    {
        string header = File.ReadAllText(RepoFile(RuntimeIdentityHeader));

        Assert.Contains($"L\"{ReadIdentityIpc("runtimeEndpointSuffix")}\"", header);
        Assert.Contains($"L\"{ReadIdentityIpc("controlEndpointSuffix")}\"", header);
        Assert.Contains($"L\"{ReadIdentityIpc("runtimeSingletonPrefix")}\"", header);
    }

    /// <summary>
    /// 把 GUID 字符串（如 famo-identity.json 里的 "54EAD76A-B864-4A6D-9C82-148E3352BEE7"）
    /// 渲染成 WeaselTSF/Globals.cpp 里那种手写的 GUID 结构体初始化字面量，逐字节对齐，
    /// 不依赖 System.Guid 的小端字节序（那与源码里的十六进制字面量顺序不一致）。
    /// </summary>
    private static string ExpectedGuidLiteral(string declPrefix, string guidValue)
    {
        string[] parts = guidValue.Split('-');
        string data1 = parts[0].ToLowerInvariant();
        string data2 = parts[1].ToLowerInvariant();
        string data3 = parts[2].ToLowerInvariant();
        string data4a = parts[3].ToLowerInvariant();
        string data4b = parts[4].ToLowerInvariant();

        List<string> data4Bytes = new();
        for (int i = 0; i < data4a.Length; i += 2) data4Bytes.Add(data4a.Substring(i, 2));
        for (int i = 0; i < data4b.Length; i += 2) data4Bytes.Add(data4b.Substring(i, 2));
        string data4 = string.Join(", ", data4Bytes.ConvertAll(b => $"0x{b}"));

        return $"{declPrefix} = {{\n    0x{data1},\n    0x{data2},\n    0x{data3},\n    {{{data4}}}}};";
    }

    private static string WithoutWhitespace(string value) =>
        string.Concat(value.Where(c => !char.IsWhiteSpace(c)));

    [Fact]
    public void IdentityScript_MapsExclusiveMutex_FromIdentityJsonTruthSource()
    {
        string script = File.ReadAllText(RepoFile(IdentityScript));

        // 旧串在替换表内，新值经 famo-identity.json 单一真相源引用（禁止脚本内硬编码新值）。
        Assert.Contains("WeaselDeployerExclusiveMutex", script);
        Assert.Contains("$($identity.ipc.deployerExclusiveMutex)", script);

        using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(RepoFile(IdentityJson)));
        Assert.Equal(
            "FamoDeployerExclusiveMutex",
            doc.RootElement.GetProperty("ipc").GetProperty("deployerExclusiveMutex").GetString());
    }

    [Fact]
    public void IdentityScript_MapsVisibleProfileNames_FromIdentityJsonTruthSource()
    {
        string script = File.ReadAllText(RepoFile(IdentityScript));

        Assert.Equal("法墨输入法", ReadIdentityBrand("displayNameZh"));
        Assert.Equal("Famo Input Method", ReadIdentityBrand("displayNameEn"));
        Assert.Contains("-Encoding UTF8", script);
        Assert.Contains("0x5C0F", script);
        Assert.Contains("0x72FC", script);
        Assert.Contains("0x6BEB", script);
        Assert.Contains("$($identity.brand.displayNameZh)", script);
        Assert.Contains("$($identity.brand.displayNameEn)", script);
    }

    [Fact]
    public void IdentityScript_MapsRegistryNamespace_FromIdentityJsonTruthSource()
    {
        string script = File.ReadAllText(RepoFile(IdentityScript));

        Assert.Equal(@"Software\Famo\InputMethod", ReadIdentityRegistry("brandKey"));
        Assert.Contains("$identity.registry.brandKey", script);
        Assert.DoesNotContain("'Software\\\\Famo\\\\Weasel'", script);
    }

    [Fact]
    public void EnsureDeployedPatch_KeepsUpstreamUpdateKey_ForIdentityRename()
    {
        string patch = File.ReadAllText(RepoFile(EnsureDeployedPatch));

        // Feature patches apply on the clean upstream pin; the WinSparkle Updates key stays
        // Software\Rime\Weasel\Updates here and is renamed to the Famo namespace by
        // apply-famo-identity.ps1 (WeaselServerApp.cpp brand-key edit) at the identity stage.
        // The patch must NOT hardcode the Famo key (that would bypass the identity layer and
        // break `git apply` on the Weasel-named pin).
        Assert.Contains(@"win_sparkle_set_registry_path(""Software\\Rime\\Weasel\\Updates"")", patch);
        Assert.DoesNotContain(@"Software\\Famo\\InputMethod\\Updates", patch);
    }

    [Fact]
    public void IdentityScript_RenamesRuntimeExeLiterals_ToMatchInstallerStaging()
    {
        // The installer stages WeaselServer.exe->FamoRuntime.exe / WeaselDeployer.exe->FamoDeploy.exe.
        // apply-famo-identity.ps1 must rename the matching source exec/detect literals, or the shipped
        // FamoRuntime.exe execs a nonexistent WeaselDeployer.exe (tray deploy / ensure-deployed / select-
        // schema dead) and TSF can't detect its renamed server. (Regression guard: M0 edaf0b2 shipped
        // the installer rename without the source rename.)
        string script = File.ReadAllText(RepoFile(IdentityScript));
        string identityJson = File.ReadAllText(RepoFile(IdentityJson));

        Assert.Contains("$serverExeOld = 'WeaselServer.exe'", script);
        Assert.Contains("$serverExeNew = $identity.brand.serverExe", script);
        Assert.Contains("$deployExeOld = 'WeaselDeployer.exe'", script);
        Assert.Contains("$deployExeNew = $identity.brand.deployerExe", script);
        Assert.Contains("\"serverExe\": \"FamoRuntime.exe\"", identityJson);
        Assert.Contains("\"deployerExe\": \"FamoDeploy.exe\"", identityJson);
    }

    [Fact]
    public void IdentityScript_ExclusiveMutexEntry_PrecedesPlainDeployerMutexEntry()
    {
        string script = File.ReadAllText(RepoFile(IdentityScript));

        // 长串（WeaselDeployerExclusiveMutex）条目必须排在短串（WeaselDeployerMutex）之前：
        // 若两条替换未来落入同一文件清单，先替换子串会破坏长串的匹配。
        int exclusive = script.IndexOf("WeaselDeployerExclusiveMutex", StringComparison.Ordinal);
        int plain = script.IndexOf("WeaselDeployerMutex", StringComparison.Ordinal);
        Assert.True(exclusive >= 0, "identity script must map WeaselDeployerExclusiveMutex");
        Assert.True(plain >= 0, "identity script must map WeaselDeployerMutex");
        Assert.True(exclusive < plain,
            "WeaselDeployerExclusiveMutex entry must precede WeaselDeployerMutex entry (longer string first)");
    }

    [Fact]
    public void IdentityScript_ProcessesBothFilesHoldingExclusiveMutex()
    {
        string script = File.ReadAllText(RepoFile(IdentityScript));

        // 上游 WeaselDeployerExclusiveMutex 的两处宿主（deployer 单实例 + TSF 拉起判定）都要在清单内。
        Assert.Contains("'WeaselTSF/WeaselTSF.cpp'", script);
        Assert.Contains("'WeaselDeployer/WeaselDeployer.cpp'", script);
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
            dir = Path.GetDirectoryName(dir);
        }
        throw new FileNotFoundException(relativePath);
    }
}
