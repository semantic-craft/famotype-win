using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace Famo.Settings.Core;

/// <summary>
/// 把法墨 TIP 加入/移出【当前用户】的输入法列表（input.dll!InstallLayoutOrTip，
/// 即 Win+Space / 语言设置里可见可切）。上游 WeaselSetup 注册后就做这一步，
/// 法墨改用 regsvr32（仅机器级 CLSID/profile 注册）后曾缺失——注册 ≠ 进列表。
/// 红线：提权安装段绝不写用户配置，故本步骤只能以原始用户身份在首启链路
/// （--seed-only）执行；卸载侧经 --remove-input-tip 反向移除。
/// </summary>
public static class InputMethodList
{
    /// <summary>法墨 TIP 串：LANGID(zh-CN 0804):{文本服务 CLSID}{语言 profile GUID}。
    /// GUID 单一真相源是 weasel-fork/famo-identity.json；契约测试锁两者一致。</summary>
    public const string FamoTip =
        "0804:{54EAD76A-B864-4A6D-9C82-148E3352BEE7}{0158C2BA-4E96-4BA8-B505-E1BBEBB3FA33}";

    private const uint IlotUninstall = 0x00000001; // ILOT_UNINSTALL
    private const uint TfProfileTypeInputProcessor = 0x00000001;
    private const uint TfIppmfEnableProfile = 0x00000001;
    private const uint TfIppmfDontCareCurrentInputLanguage = 0x00000004;
    private const uint TfIppmfForProcess = 0x10000000;
    private const uint TfIppmfForSession = 0x20000000;
    private static readonly Guid InputProcessorProfilesClsid = new("33C53A50-F456-4884-B049-85FD643ECFED");

    [DllImport(
        "input.dll",
        CharSet = CharSet.Unicode,
        ExactSpelling = true,
        SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool InstallLayoutOrTip(string psz, uint dwFlags);

    [ComImport]
    [Guid("71C6E74C-0F28-11D8-A82A-00065B84435C")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface ITfInputProcessorProfileMgr
    {
        [PreserveSig]
        int ActivateProfile(
            uint dwProfileType,
            ushort langid,
            ref Guid clsid,
            ref Guid guidProfile,
            IntPtr hkl,
            uint dwFlags);
    }

    /// <summary>加入当前用户输入法列表。幂等（已在列表时系统自行去重）；
    /// 失败（input.dll 缺失/组策略锁定等）返回 false，绝不抛——不阻断 seed/deploy 主流程。
    /// 失败可见：落一行日志到 %LOCALAPPDATA%\Famo\log\（P1-B，此前 catch 静默吞错）。</summary>
    public static bool EnsureFamoInUserList(bool logFailures = true)
    {
        int lastError = 0;
        Exception? lastException = null;
        bool result = RetryEnsureFamoInUserList(
            install: () =>
            {
                try
                {
                    bool installed = InstallLayoutOrTip(FamoTip, 0);
                    if (!installed)
                    {
                        lastError = Marshal.GetLastPInvokeError();
                    }
                    return installed;
                }
                catch (Exception ex)
                {
                    lastException = ex;
                    return false;
                }
            },
            isAlreadyPresent: () =>
                TryIsFamoInUserList(out bool present) && present,
            delay: Thread.Sleep,
            maxAttempts: 20,
            delayMilliseconds: 500);
        if (!result && logFailures)
        {
            string detail = lastException is null
                ? $"Win32={lastError}"
                : lastException.Message;
            FamoLog.Append(
                $"InstallLayoutOrTip(install) failed after retry: {detail}");
        }
        return result;
    }

    internal static bool RetryEnsureFamoInUserList(
        Func<bool> install,
        Func<bool> isAlreadyPresent,
        Action<int> delay,
        int maxAttempts,
        int delayMilliseconds)
    {
        ArgumentOutOfRangeException.ThrowIfLessThan(maxAttempts, 1);
        ArgumentOutOfRangeException.ThrowIfNegative(delayMilliseconds);

        for (int attempt = 1; attempt <= maxAttempts; attempt++)
        {
            _ = install();
            if (isAlreadyPresent())
            {
                return true;
            }
            if (attempt < maxAttempts)
            {
                delay(delayMilliseconds);
            }
        }
        return false;
    }

    /// <summary>切换当前桌面到法墨。失败只记日志：输入法已进列表时，用户仍可 Win+Space 手动切换。</summary>
    public static bool ActivateFamoForCurrentDesktop(bool logFailures = true)
    {
        if (!OperatingSystem.IsWindows())
        {
            if (logFailures)
                FamoLog.Append("ActivateProfile skipped: Windows-only API unavailable");
            return false;
        }

        object? instance = null;
        try
        {
            (ushort langid, Guid clsid, Guid profile) = ParseFamoTip();
            Type comType = Type.GetTypeFromCLSID(InputProcessorProfilesClsid, throwOnError: true)!;
            instance = Activator.CreateInstance(comType);
            if (instance is not ITfInputProcessorProfileMgr manager)
            {
                if (logFailures)
                    FamoLog.Append("ActivateProfile failed: TF_InputProcessorProfiles unavailable");
                return false;
            }

            uint flags = TfIppmfEnableProfile
                | TfIppmfDontCareCurrentInputLanguage
                | TfIppmfForProcess
                | TfIppmfForSession;
            int hr = manager.ActivateProfile(TfProfileTypeInputProcessor, langid, ref clsid, ref profile, IntPtr.Zero, flags);
            if (hr == 0) return true;

            if (logFailures)
                FamoLog.Append($"ActivateProfile failed: 0x{hr:X8}");
            return false;
        }
        catch (Exception ex)
        {
            if (logFailures)
                FamoLog.Append($"ActivateProfile failed: {ex.Message}");
            return false;
        }
        finally
        {
            if (OperatingSystem.IsWindows() && instance is not null && Marshal.IsComObject(instance))
            {
                Marshal.FinalReleaseComObject(instance);
            }
        }
    }

    /// <summary>从当前用户输入法列表移除（卸载用），只动法墨这一条。失败返回 false，不抛。</summary>
    public static bool RemoveFamoFromUserList()
    {
        try { return InstallLayoutOrTip(FamoTip, IlotUninstall); }
        catch (Exception ex)
        {
            FamoLog.Append($"InstallLayoutOrTip(uninstall) failed: {ex.Message}");
            return false;
        }
    }

    /// <summary>
    /// Read-only probe of the Windows per-user language-list store. Windows
    /// records TIP identifiers as exact value names below User Profile language
    /// subkeys; unrelated value data is intentionally ignored.
    /// </summary>
    public static bool TryIsFamoInUserList(out bool present)
    {
        present = false;
        if (!OperatingSystem.IsWindows())
        {
            return false;
        }
        try
        {
            using RegistryKey? root = Registry.CurrentUser.OpenSubKey(
                @"Control Panel\International\User Profile", writable: false);
            if (root is null)
            {
                return true;
            }
            return Scan(root, depth: 0, ref present);
        }
        catch
        {
            present = false;
            return false;
        }

        static bool Scan(RegistryKey key, int depth, ref bool found)
        {
            if (ContainsFamoTipValueName(key.GetValueNames()))
            {
                found = true;
                return true;
            }
            if (depth >= 2)
            {
                return true;
            }
            foreach (string subkeyName in key.GetSubKeyNames())
            {
                using RegistryKey? subkey = key.OpenSubKey(subkeyName, writable: false);
                if (subkey is null || !Scan(subkey, depth + 1, ref found))
                {
                    return false;
                }
                if (found)
                {
                    return true;
                }
            }
            return true;
        }
    }

    internal static bool ContainsFamoTipValueName(
        IEnumerable<string> valueNames) =>
        valueNames.Any(valueName => string.Equals(
            valueName, FamoTip, StringComparison.OrdinalIgnoreCase));

    private static (ushort LangId, Guid Clsid, Guid Profile) ParseFamoTip()
    {
        int colon = FamoTip.IndexOf(':');
        int secondOpen = FamoTip.IndexOf('{', colon + 2);
        ushort langid = Convert.ToUInt16(FamoTip[..colon], 16);
        Guid clsid = Guid.Parse(FamoTip.Substring(colon + 1, secondOpen - colon - 1));
        Guid profile = Guid.Parse(FamoTip[secondOpen..]);
        return (langid, clsid, profile);
    }
}
