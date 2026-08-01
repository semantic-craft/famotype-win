using Microsoft.Win32;

namespace Famo.Settings.Core;

/// <summary>启动期 TIP 自愈结果（#41）。</summary>
public enum TipSelfHealOutcome
{
    /// <summary>InstallState 非 Ready（含缺键）：PendingReboot/RolledBack/已卸载阶段 TIP 本就应缺席，不探不修。</summary>
    SkippedInstallState,
    /// <summary>连续 2 次稳定回读在列，零写入。</summary>
    Healthy,
    /// <summary>修复后连续 2 次稳定回读确认。</summary>
    Repaired,
    /// <summary>走满有界重试仍未稳定（如组策略拒绝）；只记日志，不影响打字。</summary>
    Failed,
}

/// <summary>
/// 启动期 TIP 自愈（#41）：安装器 Ready 路径只证明安装时点状态，之后系统或
/// 用户语言列表变化可能移除法墨 TIP。本 helper 收敛安装器
/// EnsureStableUserProfileState（famo-setup.iss）用户列表腿的语义——
/// probe → add → 连续 2 次稳定回读，6 轮×2s 有界——由 Settings GUI 启动
/// （App.OnLaunched 后台线程）与 Runtime 启动（runtime_main.cpp 壳到
/// FamoSettings.exe 无头模式）调用。门只认 InstallState=Ready。
/// </summary>
public static class TipSelfHeal
{
    /// <summary>真实接线入口：探针/修复复用 InputMethodList（与安装器
    /// --is-input-tip / --add-input-tip 同一份实现）。绝不抛；仅
    /// Repaired/Failed 落日志——健康与跳过每次启动都会发生，不刷日志。</summary>
    public static TipSelfHealOutcome RunAtStartup(string source)
    {
        try
        {
            TipSelfHealOutcome outcome = Run(
                readInstallState: ReadInstallState,
                isPresent: () =>
                    InputMethodList.TryIsFamoInUserList(out bool present) && present,
                repair: () => InputMethodList.EnsureFamoInUserList(),
                delay: Thread.Sleep);
            if (outcome is TipSelfHealOutcome.Repaired or TipSelfHealOutcome.Failed)
            {
                FamoLog.Append($"tip self-heal ({source}): {outcome}");
            }
            return outcome;
        }
        catch (Exception ex)
        {
            FamoLog.Append($"tip self-heal ({source}) threw: {ex.Message}");
            return TipSelfHealOutcome.Failed;
        }
    }

    /// <summary>安装器同款有界稳定循环（6 轮×2s，连续 2 次稳定回读）。
    /// 非 Ready 一律不探不修；健康路径零写入。</summary>
    internal static TipSelfHealOutcome Run(
        Func<string?> readInstallState,
        Func<bool> isPresent,
        Func<bool> repair,
        Action<int> delay,
        int maxAttempts = 6,
        int delayMilliseconds = 2000)
    {
        if (!string.Equals(readInstallState(), "Ready", StringComparison.Ordinal))
        {
            return TipSelfHealOutcome.SkippedInstallState;
        }

        int stableReadbacks = 0;
        bool repaired = false;
        for (int attempt = 1; attempt <= maxAttempts; attempt++)
        {
            if (isPresent())
            {
                stableReadbacks++;
                if (stableReadbacks >= 2)
                {
                    return repaired
                        ? TipSelfHealOutcome.Repaired
                        : TipSelfHealOutcome.Healthy;
                }
            }
            else
            {
                stableReadbacks = 0;
                repaired |= repair(); // repair 自身有界（20×500ms）且失败已落日志
            }
            if (attempt < maxAttempts)
            {
                delay(delayMilliseconds);
            }
        }
        return TipSelfHealOutcome.Failed;
    }

    /// <summary>读 HKLM\SOFTWARE\Famo\InputMethod（64 位视图）的 InstallState；
    /// 缺键/异常返回 null（与已卸载同治：不修）。与原生
    /// install_state.cpp 的读取语义一致，但门更紧：只认 Ready，
    /// 无任何放宽开关。</summary>
    private static string? ReadInstallState()
    {
        if (!OperatingSystem.IsWindows())
        {
            return null;
        }
        try
        {
            using RegistryKey hklm64 = RegistryKey.OpenBaseKey(
                RegistryHive.LocalMachine, RegistryView.Registry64);
            using RegistryKey? key = hklm64.OpenSubKey(
                @"SOFTWARE\Famo\InputMethod", writable: false);
            return key?.GetValue("InstallState") as string;
        }
        catch
        {
            return null;
        }
    }
}
