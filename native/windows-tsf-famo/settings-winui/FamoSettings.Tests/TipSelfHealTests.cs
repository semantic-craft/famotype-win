using System.Text.RegularExpressions;
using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

/// <summary>
/// #41 启动期 TIP 自愈契约：安装器 Ready 路径（probe → add → 连续 2 次稳定回读，
/// famo-setup.iss EnsureStableUserProfileState）的语义收敛到共享 helper，
/// 由 Settings GUI 启动与 Runtime 启动调用。门只认 InstallState=Ready——
/// PendingReboot/RolledBack/已卸载阶段 TIP 本就应缺席，一律不修。
/// </summary>
public sealed class TipSelfHealTests
{
    // ── 状态矩阵：非 Ready 一律不探不修 ──

    [Theory]
    [InlineData("Activating")]
    [InlineData("PendingReboot")]
    [InlineData("RolledBack")]
    [InlineData("")]
    [InlineData(null)] // 键/值缺失 = 已卸载或从未安装
    public void Run_NonReadyInstallState_NeverProbesNorRepairs(string? state)
    {
        int probes = 0, repairs = 0, delays = 0;

        TipSelfHealOutcome outcome = TipSelfHeal.Run(
            readInstallState: () => state,
            isPresent: () => { probes++; return false; },
            repair: () => { repairs++; return true; },
            delay: _ => delays++);

        Assert.Equal(TipSelfHealOutcome.SkippedInstallState, outcome);
        Assert.Equal(0, probes);
        Assert.Equal(0, repairs);
        Assert.Equal(0, delays);
    }

    // ── 幂等：健康时零写入（连续 2 次稳定回读后即 Healthy）──

    [Fact]
    public void Run_ReadyAndPresent_TwoStableReadbacksNoWrites()
    {
        int probes = 0, repairs = 0;
        List<int> delays = [];

        TipSelfHealOutcome outcome = TipSelfHeal.Run(
            readInstallState: () => "Ready",
            isPresent: () => { probes++; return true; },
            repair: () => { repairs++; return true; },
            delay: delays.Add);

        Assert.Equal(TipSelfHealOutcome.Healthy, outcome);
        Assert.Equal(2, probes);       // 双稳定回读
        Assert.Equal(0, repairs);      // 零写入
        Assert.Single(delays);         // 两次回读之间一次间隔
    }

    // ── 故障注入：TIP 被移除 → 修复一次 → 双回读确认 ──

    [Fact]
    public void Run_ReadyAndAbsent_RepairsOnceThenConfirmsTwice()
    {
        Queue<bool> presence = new([false, true, true]);
        int repairs = 0;
        List<int> delays = [];

        TipSelfHealOutcome outcome = TipSelfHeal.Run(
            readInstallState: () => "Ready",
            isPresent: presence.Dequeue,
            repair: () => { repairs++; return true; },
            delay: delays.Add);

        Assert.Equal(TipSelfHealOutcome.Repaired, outcome);
        Assert.Equal(1, repairs);
        Assert.Equal(2, delays.Count);
    }

    // ── 回读中途翻覆：稳定计数清零并再修（安装器同款语义）──

    [Fact]
    public void Run_FlappingReadback_ResetsStableCountAndRepairsAgain()
    {
        Queue<bool> presence = new([true, false, true, true]);
        int repairs = 0;

        TipSelfHealOutcome outcome = TipSelfHeal.Run(
            readInstallState: () => "Ready",
            isPresent: presence.Dequeue,
            repair: () => { repairs++; return true; },
            delay: _ => { });

        Assert.Equal(TipSelfHealOutcome.Repaired, outcome);
        Assert.Equal(1, repairs);
    }

    // ── 有界重试：策略拒绝（修复恒败）→ 恰好走满安装器同款 6 轮×2s 后失败 ──

    [Fact]
    public void Run_RepairAlwaysDenied_FailsBoundedWithInstallerConstants()
    {
        int probes = 0, repairs = 0;
        List<int> delays = [];

        TipSelfHealOutcome outcome = TipSelfHeal.Run(
            readInstallState: () => "Ready",
            isPresent: () => { probes++; return false; },
            repair: () => { repairs++; return false; },
            delay: delays.Add);

        Assert.Equal(TipSelfHealOutcome.Failed, outcome);
        Assert.Equal(6, probes);                       // 安装器同款 6 轮
        Assert.Equal(6, repairs);
        Assert.Equal([2000, 2000, 2000, 2000, 2000], delays); // 2s 间隔
    }

    // ── 门读取器：HKLM 64 位视图 SOFTWARE\Famo\InputMethod\InstallState ──

    [Fact]
    public void InstallStateReader_UsesHklm64BitViewAndExactKey()
    {
        string source = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/settings-winui/FamoSettings.Core/TipSelfHeal.cs"));

        Assert.Contains("RegistryView.Registry64", source);
        Assert.Contains(@"SOFTWARE\Famo\InputMethod", source);
        Assert.Contains("InstallState", source);
        // 门只认 Ready；Activating 也不修（对比 install_state.cpp 的 allow_activating 开关）。
        Assert.Contains("\"Ready\"", source);
        Assert.DoesNotContain("Activating", source);
    }

    // ── 启动入口：真实探针/修复接线复用 InputMethodList，仅修复/失败落日志 ──

    [Fact]
    public void RunAtStartup_ReusesInputMethodListAndLogsOnlyRepairOutcomes()
    {
        string source = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/settings-winui/FamoSettings.Core/TipSelfHeal.cs"));

        Assert.Contains("InputMethodList.TryIsFamoInUserList", source);
        Assert.Contains("InputMethodList.EnsureFamoInUserList", source);
        // 修复成功/失败要留痕；健康与跳过不刷日志（每次启动都跑）。
        Assert.Contains("TipSelfHealOutcome.Repaired or TipSelfHealOutcome.Failed", source);
        Assert.Contains("FamoLog.Append", source);
    }

    // ── Settings 接线：GUI 路径后台执行；无头 seed/demo 早退不重复调 ──

    [Fact]
    public void SettingsStartup_RunsSelfHealOffUiThreadAfterHeadlessExits()
    {
        string app = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/settings-winui/FamoSettings/App.xaml.cs"));

        Assert.Matches(
            new Regex(@"Task\.Run\([^;]*TipSelfHeal\.RunAtStartup", RegexOptions.Singleline),
            app);
        // 自愈调用必须排在 --seed-only 与 --demo-appearance 两个早退分支之后。
        int heal = app.IndexOf("TipSelfHeal.RunAtStartup", StringComparison.Ordinal);
        int seedOnly = app.IndexOf("--seed-only", StringComparison.Ordinal);
        int demo = app.IndexOf("--demo-appearance", StringComparison.Ordinal);
        Assert.True(seedOnly >= 0 && heal > seedOnly,
            "self-heal must come after the --seed-only headless early return");
        Assert.True(demo >= 0 && heal > demo,
            "self-heal must come after the --demo-appearance headless early return");
    }

    // ── Runtime 接线：安装期可在 Activating 启动，但必须有界等到 Ready，
    //    立即委托一次后还要跨过安装结束时的 Windows 输入源重整窗口再复查；
    //    两次都复用共享 helper，不能复制第三套探针/修复循环 ──

    [Fact]
    public void RuntimeStartup_RechecksAfterReadySettles()
    {
        string main = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/runtime-protocol/src/runtime_main.cpp"));
        string program = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/settings-winui/FamoSettings/Program.cs"));

        int singleton = main.IndexOf("ERROR_ALREADY_EXISTS", StringComparison.Ordinal);
        int activating = main.IndexOf(
            "ProductionInstallAllowed(ModuleDirectory(), true)",
            singleton,
            StringComparison.Ordinal);
        Assert.True(singleton >= 0 && activating > singleton,
            "the singleton runtime must recognize only its own Activating projection while it waits");
        int ready = main.IndexOf(
            "ProductionInstallAllowed(ModuleDirectory(), false)",
            activating,
            StringComparison.Ordinal);
        Assert.True(ready > activating,
            "the runtime must not delegate until the projection reaches Ready");
        int delegated = main.IndexOf("--tip-self-heal", ready,
            StringComparison.Ordinal);
        Assert.True(
            ready < delegated,
            "the singleton runtime must wait through its own Activating projection, then delegate only after Ready");
        int settledReady = main.IndexOf(
            "ProductionInstallAllowed(ModuleDirectory(), false)",
            delegated,
            StringComparison.Ordinal);
        int redelegated = main.IndexOf(
            "--tip-self-heal",
            delegated + 1,
            StringComparison.Ordinal);
        Assert.True(settledReady > delegated && redelegated > settledReady,
            "the runtime must remain Ready through a bounded post-install settling window, then delegate a second time");
        Assert.Contains("kTipSelfHealReadyAttempts", main);
        Assert.Contains("kTipSelfHealPostReadyAttempts", main);
        Assert.Contains("kTipSelfHealPostReadyDelayMs", main);
        Assert.Contains(".detach()", main);
        Assert.Contains("tip-selfheal", main);
        Assert.DoesNotContain("--is-input-tip", main);
        Assert.DoesNotContain("--add-input-tip", main);

        int dispatch = program.IndexOf(
            "HasFlag(args, \"--tip-self-heal\")",
            StringComparison.Ordinal);
        int xaml = program.IndexOf(
            "WinRT.ComWrappersSupport.InitializeComWrappers",
            StringComparison.Ordinal);
        Assert.True(dispatch >= 0 && dispatch < xaml,
            "runtime self-heal must stay in the headless path");
        Assert.Contains("TipSelfHeal.RunAtStartup(\"runtime\")", program);
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
