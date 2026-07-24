using System.Globalization;
using System.Text;

namespace Famo.Settings.Core;

/// <summary>
/// Opt-in local timing log for support builds. Disabled unless FAMO_LOCAL_TIMING=1/true/yes.
/// Entries are small, redacted to structured fields, rate-limited, and capped on disk.
/// </summary>
public static class FamoTimingLog
{
    public const string EnableEnvironmentVariable = "FAMO_LOCAL_TIMING";
    public const int MaxLogBytes = 128 * 1024;
    private static readonly object Gate = new();
    private static DateTime _lastWriteUtc = DateTime.MinValue;

    internal static TimeSpan MinWriteInterval { get; set; } = TimeSpan.FromSeconds(1);
    internal static string? OverrideLogDirForTests { get; set; }
    internal static bool? ForceEnabledForTests { get; set; }

    public static bool IsEnabled()
    {
        if (ForceEnabledForTests.HasValue)
        {
            return ForceEnabledForTests.Value;
        }

        string? value = Environment.GetEnvironmentVariable(EnableEnvironmentVariable);
        return string.Equals(value, "1", StringComparison.OrdinalIgnoreCase)
            || string.Equals(value, "true", StringComparison.OrdinalIgnoreCase)
            || string.Equals(value, "yes", StringComparison.OrdinalIgnoreCase);
    }

    public static void Append(
        string component,
        string operation,
        TimeSpan elapsed,
        string status,
        string? logDir = null)
    {
        if (!IsEnabled())
        {
            return;
        }

        try
        {
            lock (Gate)
            {
                DateTime now = DateTime.UtcNow;
                if (now - _lastWriteUtc < MinWriteInterval)
                {
                    return;
                }

                _lastWriteUtc = now;
                logDir ??= OverrideLogDirForTests ?? Path.Combine(FamoPaths.FamoDir, "log");
                Directory.CreateDirectory(logDir);

                string path = Path.Combine(logDir, "famo-timing.log");
                EnsureBounded(path);

                string line = string.Create(
                    CultureInfo.InvariantCulture,
                    $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} component={Clean(component)} operation={Clean(operation)} elapsedMs={elapsed.TotalMilliseconds:0.###} status={Clean(status)}{Environment.NewLine}");
                File.AppendAllText(path, line, Encoding.UTF8);
            }
        }
        catch
        {
            // Timing diagnostics must never affect settings or typing.
        }
    }

    internal static void ResetForTests()
    {
        lock (Gate)
        {
            _lastWriteUtc = DateTime.MinValue;
            MinWriteInterval = TimeSpan.FromSeconds(1);
            OverrideLogDirForTests = null;
            ForceEnabledForTests = null;
        }
    }

    private static void EnsureBounded(string path)
    {
        if (!File.Exists(path))
        {
            return;
        }

        var info = new FileInfo(path);
        if (info.Length <= MaxLogBytes)
        {
            return;
        }

        File.WriteAllText(
            path,
            $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} component=timing operation=truncate elapsedMs=0 status=previous-log-truncated{Environment.NewLine}",
            Encoding.UTF8);
    }

    private static string Clean(string value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return "unknown";
        }

        var builder = new StringBuilder(value.Length);
        foreach (char c in value)
        {
            builder.Append(char.IsLetterOrDigit(c) || c is '/' or '-' or '_' or '.' or ':' ? c : '_');
        }

        return builder.Length == 0 ? "unknown" : builder.ToString();
    }
}
