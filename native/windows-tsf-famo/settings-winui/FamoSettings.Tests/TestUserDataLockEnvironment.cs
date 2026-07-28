using System.Runtime.CompilerServices;
using Famo.Settings.Core;

namespace Famo.Settings.Tests;

internal static class TestUserDataLockEnvironment
{
    internal static string LocalAppDataRoot { get; } = Path.Combine(
        Path.GetTempPath(),
        $"famo-test-lock-local-data-{Environment.ProcessId}");

    [ModuleInitializer]
    internal static void Initialize()
    {
        UserDataTransactionLock.LocalAppDataOverrideForTests =
            LocalAppDataRoot;
        AppDomain.CurrentDomain.ProcessExit += (_, _) =>
        {
            try
            {
                if (Directory.Exists(LocalAppDataRoot))
                {
                    Directory.Delete(LocalAppDataRoot, recursive: true);
                }
            }
            catch
            {
                // Test cleanup is best effort and must not mask test results.
            }
        };
    }
}
