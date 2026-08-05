using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace Famo.Settings.Core;

/// <summary>
/// 把法墨 TIP 加入/移出【当前用户】的输入法列表（input.dll!InstallLayoutOrTip
/// 引导 + Windows International 模块持久化，即 Win+Space / 语言设置里可见可切）。
/// 上游 WeaselSetup 注册后就做这一步，
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

    /// <summary>只用得到 enable 位的两个方法，其余槽位按 vtable 顺序占位。</summary>
    [ComImport]
    [Guid("1F02B6C5-7842-4EE6-8A0B-9A24183A95CA")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface ITfInputProcessorProfiles
    {
        [PreserveSig] int Register(ref Guid rclsid);
        [PreserveSig] int Unregister(ref Guid rclsid);
        [PreserveSig] int AddLanguageProfile(
            ref Guid rclsid, ushort langid, ref Guid guidProfile,
            [MarshalAs(UnmanagedType.LPWStr)] string pchDesc, uint cchDesc,
            [MarshalAs(UnmanagedType.LPWStr)] string pchIconFile,
            uint cchFile, uint uIconIndex);
        [PreserveSig] int RemoveLanguageProfile(
            ref Guid rclsid, ushort langid, ref Guid guidProfile);
        [PreserveSig] int EnumInputProcessorInfo(out IntPtr ppEnum);
        [PreserveSig] int GetDefaultLanguageProfile(
            ushort langid, ref Guid catid, out Guid pclsid, out Guid pguidProfile);
        [PreserveSig] int SetDefaultLanguageProfile(
            ushort langid, ref Guid rclsid, ref Guid guidProfiles);
        [PreserveSig] int ActivateLanguageProfile(
            ref Guid rclsid, ushort langid, ref Guid guidProfiles);
        [PreserveSig] int GetActiveLanguageProfile(
            ref Guid rclsid, out ushort plangid, out Guid pguidProfile);
        [PreserveSig] int GetLanguageProfileDescription(
            ref Guid rclsid, ushort langid, ref Guid guidProfile, out IntPtr pbstrProfile);
        [PreserveSig] int GetCurrentLanguage(out ushort plangid);
        [PreserveSig] int ChangeCurrentLanguage(ushort langid);
        [PreserveSig] int GetLanguageList(out IntPtr ppLangId, out uint pulCount);
        [PreserveSig] int EnumLanguageProfiles(ushort langid, out IntPtr ppEnum);
        [PreserveSig] int EnableLanguageProfile(
            ref Guid rclsid, ushort langid, ref Guid guidProfile,
            [MarshalAs(UnmanagedType.Bool)] bool fEnable);
        [PreserveSig] int IsEnabledLanguageProfile(
            ref Guid rclsid, ushort langid, ref Guid guidProfile,
            [MarshalAs(UnmanagedType.Bool)] out bool pfEnable);
    }

    /// <summary>加入当前用户输入法列表。幂等（已在列表时系统自行去重）；
    /// 失败（input.dll 缺失/组策略锁定等）返回 false，绝不抛——不阻断 seed/deploy 主流程。
    /// 失败可见：落一行日志到 %LOCALAPPDATA%\Famo\log\（P1-B，此前 catch 静默吞错）。</summary>
    public static bool EnsureFamoInUserList(bool logFailures = true)
    {
        int lastError = 0;
        Exception? lastException = null;
        string languageListError = "";
        bool result = TryEnsureDurableFamoInUserList(
            ensureNative: () => RetryEnsureFamoInUserList(
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
                delayMilliseconds: 500),
            persistWithUserLanguageList: () =>
                EnsureFamoWithUserLanguageList(out languageListError));
        if (!result && logFailures)
        {
            string nativeDetail = lastException is null
                ? $"Win32={lastError}"
                : lastException.Message;
            FamoLog.Append(
                $"InstallLayoutOrTip(install) failed after retry: native={nativeDetail}; language-list={languageListError}");
        }
        return result;
    }

    internal static bool TryEnsureDurableFamoInUserList(
        Func<bool> ensureNative,
        Func<bool> persistWithUserLanguageList)
    {
        _ = ensureNative();
        // InstallLayoutOrTip can expose a process-local healthy readback while
        // Windows has not persisted the current-user language list. Converge
        // through the official language-list API and treat its fresh readback
        // as authoritative.
        return persistWithUserLanguageList();
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

    /// <summary>从当前用户输入法列表移除（卸载用），只动法墨这一条。
    /// ILOT_UNINSTALL 只禁用 TIP；若它仍在已启用列表中，再通过 Windows
    /// International 模块更新当前用户语言列表，并以只读探针确认。</summary>
    public static bool RemoveFamoFromUserList(bool logFailures = true)
    {
        int lastError = 0;
        Exception? lastException = null;
        string languageListError = "";
        bool result = TryRemoveFamoFromUserList(
            disable: () =>
            {
                try
                {
                    bool disabled = InstallLayoutOrTip(FamoTip, IlotUninstall);
                    if (!disabled)
                    {
                        lastError = Marshal.GetLastPInvokeError();
                    }
                    return disabled;
                }
                catch (Exception ex)
                {
                    lastException = ex;
                    return false;
                }
            },
            removeFromLanguageList: () =>
                RemoveFamoWithUserLanguageList(out languageListError),
            isStillPresent: () =>
                !TryIsFamoInUserList(out bool present) || present);
        if (!result && logFailures)
        {
            string nativeDetail = lastException is null
                ? $"Win32={lastError}"
                : lastException.Message;
            FamoLog.Append(
                $"input TIP removal failed: native={nativeDetail}; language-list={languageListError}");
        }
        return result;
    }

    internal static bool TryRemoveFamoFromUserList(
        Func<bool> disable,
        Func<bool> removeFromLanguageList,
        Func<bool> isStillPresent)
    {
        _ = disable();
        if (!isStillPresent())
        {
            return true;
        }
        _ = removeFromLanguageList();
        return !isStillPresent();
    }

    private static bool EnsureFamoWithUserLanguageList(out string error)
    {
        const string script = """
            $ErrorActionPreference = 'Stop'
            $tip = '0804:{54EAD76A-B864-4A6D-9C82-148E3352BEE7}{0158C2BA-4E96-4BA8-B505-E1BBEBB3FA33}'
            $list = Get-WinUserLanguageList
            $target = $null
            foreach ($language in $list) {
              if ([string]::Equals([string]$language.LanguageTag, 'zh-Hans-CN', [StringComparison]::OrdinalIgnoreCase) -or
                  [string]::Equals([string]$language.LanguageTag, 'zh-CN', [StringComparison]::OrdinalIgnoreCase)) {
                $target = $language
                break
              }
            }
            if ($null -eq $target) {
              foreach ($language in $list) {
                foreach ($entry in $language.InputMethodTips) {
                  if ($entry.StartsWith('0804:', [StringComparison]::OrdinalIgnoreCase)) {
                    $target = $language
                    break
                  }
                }
                if ($null -ne $target) { break }
              }
            }
            if ($null -eq $target) {
              throw 'Simplified Chinese user language is unavailable'
            }
            $present = $false
            foreach ($entry in $target.InputMethodTips) {
              if ([string]::Equals($entry, $tip, [StringComparison]::OrdinalIgnoreCase)) {
                $present = $true
                break
              }
            }
            if (-not $present) {
              $target.InputMethodTips.Add($tip)
            }
            Set-WinUserLanguageList -LanguageList $list -Force
            foreach ($language in Get-WinUserLanguageList) {
              foreach ($entry in $language.InputMethodTips) {
                if ([string]::Equals($entry, $tip, [StringComparison]::OrdinalIgnoreCase)) {
                  exit 0
                }
              }
            }
            exit 1
            """;

        return RunUserLanguageListScript(script, out error);
    }

    private static bool RemoveFamoWithUserLanguageList(out string error)
    {
        const string script = """
            $ErrorActionPreference = 'Stop'
            $tip = '0804:{54EAD76A-B864-4A6D-9C82-148E3352BEE7}{0158C2BA-4E96-4BA8-B505-E1BBEBB3FA33}'
            $list = Get-WinUserLanguageList
            $changed = $false
            foreach ($language in $list) {
              for ($index = $language.InputMethodTips.Count - 1; $index -ge 0; $index--) {
                if ([string]::Equals($language.InputMethodTips[$index], $tip, [StringComparison]::OrdinalIgnoreCase)) {
                  $language.InputMethodTips.RemoveAt($index)
                  $changed = $true
                }
              }
            }
            if ($changed) {
              Set-WinUserLanguageList -LanguageList $list -Force
            }
            foreach ($language in Get-WinUserLanguageList) {
              foreach ($entry in $language.InputMethodTips) {
                if ([string]::Equals($entry, $tip, [StringComparison]::OrdinalIgnoreCase)) {
                  exit 1
                }
              }
            }
            exit 0
            """;

        return RunUserLanguageListScript(script, out error);
    }

    private static bool RunUserLanguageListScript(string script, out string error)
    {
        error = "";
        if (!OperatingSystem.IsWindows())
        {
            error = "Windows-only International module unavailable";
            return false;
        }

        string powershell = Path.Combine(
            Environment.SystemDirectory,
            @"WindowsPowerShell\v1.0\powershell.exe");
        if (!File.Exists(powershell))
        {
            error = "Windows PowerShell not found";
            return false;
        }

        try
        {
            ProcessStartInfo startInfo = new()
            {
                FileName = powershell,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };
            foreach (string argument in new[]
            {
                "-NoLogo",
                "-NoProfile",
                "-NonInteractive",
                "-Command",
                script,
            })
            {
                startInfo.ArgumentList.Add(argument);
            }

            using Process process = new() { StartInfo = startInfo };
            if (!process.Start())
            {
                error = "Windows PowerShell did not start";
                return false;
            }
            Task<string> stdout = process.StandardOutput.ReadToEndAsync();
            Task<string> stderr = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit(30_000))
            {
                process.Kill(entireProcessTree: true);
                process.WaitForExit();
                error = "Windows PowerShell timed out";
                return false;
            }
            _ = stdout.GetAwaiter().GetResult();
            string stderrText = stderr.GetAwaiter().GetResult().Trim();
            if (process.ExitCode == 0)
            {
                return true;
            }
            error = string.IsNullOrWhiteSpace(stderrText)
                ? $"Windows PowerShell exited {process.ExitCode}"
                : stderrText;
            return false;
        }
        catch (Exception ex)
        {
            error = ex.Message;
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

    /// <summary>读当前用户的 profile 启用位（ITfInputProcessorProfiles）。
    /// 读不到（非 Windows / COM 不可用）返回 false，与"未知"同治：不修。</summary>
    public static bool TryIsFamoProfileEnabled(out bool enabled)
    {
        enabled = false;
        if (!OperatingSystem.IsWindows())
        {
            return false;
        }
        try
        {
            if (CreateProfiles() is not ITfInputProcessorProfiles profiles)
            {
                return false;
            }
            (ushort langid, Guid clsid, Guid profile) = ParseFamoTip();
            int hr = profiles.IsEnabledLanguageProfile(
                ref clsid, langid, ref profile, out bool value);
            Marshal.ReleaseComObject(profiles);
            if (hr < 0)
            {
                return false;
            }
            enabled = value;
            return true;
        }
        catch
        {
            enabled = false;
            return false;
        }
    }

    /// <summary>把当前用户的 profile 启用位置真。
    ///
    /// 机器级注册故意不写这一位：安装器是提权运行的，EnableLanguageProfile 只
    /// 写调用者的 HKCU，那会落到管理员的 hive 而不是真正的用户。所以这一步必须
    /// 由非提权的每用户链路补上，否则新装的机器上 profile 已注册、TIP 也在列表
    /// 里，却是禁用状态——语言栏能切，输入法不工作。
    /// 幂等；失败返回 false 并落日志，绝不抛。</summary>
    public static bool EnsureFamoProfileEnabled(bool logFailures = true)
    {
        if (!OperatingSystem.IsWindows())
        {
            return false;
        }
        try
        {
            if (CreateProfiles() is not ITfInputProcessorProfiles profiles)
            {
                if (logFailures)
                {
                    FamoLog.Append(
                        "EnableLanguageProfile skipped: profiles unavailable");
                }
                return false;
            }
            (ushort langid, Guid clsid, Guid profile) = ParseFamoTip();
            int hr = profiles.EnableLanguageProfile(
                ref clsid, langid, ref profile, true);
            Marshal.ReleaseComObject(profiles);
            if (hr < 0)
            {
                if (logFailures)
                {
                    FamoLog.Append(
                        $"EnableLanguageProfile failed: hr=0x{hr:X8}");
                }
                return false;
            }
            return TryIsFamoProfileEnabled(out bool enabled) && enabled;
        }
        catch (Exception ex)
        {
            if (logFailures)
            {
                FamoLog.Append($"EnableLanguageProfile threw: {ex.Message}");
            }
            return false;
        }
    }

    private static object? CreateProfiles()
    {
        Type? type = Type.GetTypeFromCLSID(InputProcessorProfilesClsid);
        return type is null ? null : Activator.CreateInstance(type);
    }

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
