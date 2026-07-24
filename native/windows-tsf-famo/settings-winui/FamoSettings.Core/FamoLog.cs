namespace Famo.Settings.Core;

/// <summary>
/// 最简失败日志：一行 append 到 %LOCALAPPDATA%\Famo\log\famo-settings.log。
/// 只用于「失败原本被静默吞掉」的路径（--seed-only / InstallLayoutOrTip），
/// 自身绝不抛——日志失败不得再伤 seed/加列表主流程。
/// </summary>
public static class FamoLog
{
    /// <summary>追加一行「时间戳 + 消息」。logDir 缺省为 %LOCALAPPDATA%\Famo\log（测试可注入临时目录）。</summary>
    public static void Append(string message, string? logDir = null)
    {
        // ponytail: 单文件 append，无轮转；日志量大了再上轮转。
        try
        {
            logDir ??= Path.Combine(FamoPaths.FamoDir, "log");
            Directory.CreateDirectory(logDir);
            File.AppendAllText(
                Path.Combine(logDir, "famo-settings.log"),
                $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {message}{Environment.NewLine}");
        }
        catch
        {
            // 日志失败不阻断调用方。
        }
    }
}
