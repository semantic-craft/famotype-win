using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

/// <summary>P1-B 失败可见：静默吞错路径必须落一行日志（时间戳+消息），且日志自身绝不抛。</summary>
public sealed class FamoLogTests
{
    [Fact]
    public void Append_WritesTimestampedLine_AndCreatesLogDir()
    {
        string dir = Path.Combine(Path.GetTempPath(), "famo-log-tests-" + Guid.NewGuid().ToString("N"));
        try
        {
            FamoLog.Append("InstallLayoutOrTip(install) failed: boom", dir);

            string text = File.ReadAllText(Path.Combine(dir, "famo-settings.log"));
            Assert.Contains("InstallLayoutOrTip(install) failed: boom", text);
            Assert.Matches(@"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} ", text);
        }
        finally
        {
            if (Directory.Exists(dir)) Directory.Delete(dir, recursive: true);
        }
    }

    [Fact]
    public void Append_AppendsSecondLine_DoesNotOverwrite()
    {
        string dir = Path.Combine(Path.GetTempPath(), "famo-log-tests-" + Guid.NewGuid().ToString("N"));
        try
        {
            FamoLog.Append("first", dir);
            FamoLog.Append("second", dir);

            string[] lines = File.ReadAllLines(Path.Combine(dir, "famo-settings.log"));
            Assert.Equal(2, lines.Length);
        }
        finally
        {
            if (Directory.Exists(dir)) Directory.Delete(dir, recursive: true);
        }
    }

    [Fact]
    public void Append_NeverThrows_OnUnwritableTarget()
    {
        // 日志失败不得再伤 seed/加列表主流程。
        FamoLog.Append("x", "\0<invalid>");
    }

    /// <summary>文本契约（沿用既有范式）：两处曾静默吞错的 catch 现在都调用 FamoLog。</summary>
    [Fact]
    public void SilentFailureCatches_NowWriteOneLogLine()
    {
        string iml = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/settings-winui/FamoSettings.Core/InputMethodList.cs"));
        Assert.DoesNotContain("catch { return false; }", iml);
        Assert.Contains(@"FamoLog.Append($""InstallLayoutOrTip(install) failed", iml);
        Assert.Contains(@"FamoLog.Append($""InstallLayoutOrTip(uninstall) failed", iml);

        string program = File.ReadAllText(RepoFile(
            "native/windows-tsf-famo/settings-winui/FamoSettings/Program.cs"));
        int seed = program.IndexOf("private static int RunSeedOnly", StringComparison.Ordinal);
        int log = program.IndexOf(@"FamoLog.Append($""--seed-only failed", StringComparison.Ordinal);
        Assert.True(seed >= 0 && log > seed,
            "RunSeedOnly catch must log the failure (Inno [Run] ignores exit codes; without a log line first-launch failure is invisible)");
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
