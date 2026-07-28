namespace Famo.Settings.Core;

public readonly record struct FirstLaunchSeedResult(
    string SourceDir,
    string TargetDir,
    int PayloadFiles,
    int Copied,
    int Skipped);

/// <summary>
/// First-launch engine config seed for shape A: copy installed shared data into
/// the current user's %LOCALAPPDATA%\Famo before deploying. Never touches
/// %AppData%\Rime.
/// </summary>
public static class FirstLaunchSeeder
{
    /// <summary>
    /// Settings is installed under {app}\settings, while immutable shared data
    /// is installed under {app}\data.
    /// </summary>
    public static string ResolveInstalledDataDir(string? settingsBaseDirectory = null)
    {
        string baseDir = Path.GetFullPath(settingsBaseDirectory ?? AppContext.BaseDirectory);
        string flatCandidate = Path.GetFullPath(Path.Combine(baseDir, "data"));
        if (Directory.Exists(flatCandidate))
        {
            return flatCandidate;
        }

        string candidate = Path.GetFullPath(Path.Combine(baseDir, "..", "data"));
        if (!Directory.Exists(candidate))
        {
            throw new DirectoryNotFoundException($"Famo installed data directory not found: {candidate}");
        }
        return candidate;
    }

    public static FirstLaunchSeedResult SeedFromInstalledData(
        string? settingsBaseDirectory = null,
        string? targetDir = null,
        bool force = false) =>
        Seed(ResolveInstalledDataDir(settingsBaseDirectory), targetDir ?? FamoPaths.FamoDir, force);

    public static FirstLaunchSeedResult Seed(string sourceDir, string targetDir, bool force = false)
    {
        if (string.IsNullOrWhiteSpace(sourceDir))
        {
            throw new ArgumentException("Source directory is required.", nameof(sourceDir));
        }
        if (string.IsNullOrWhiteSpace(targetDir))
        {
            throw new ArgumentException("Target directory is required.", nameof(targetDir));
        }
        if (!Directory.Exists(sourceDir))
        {
            throw new DirectoryNotFoundException($"Famo seed source not found: {sourceDir}");
        }

        string fullSource = Path.GetFullPath(sourceDir);
        string fullTarget = Path.GetFullPath(targetDir);
        using IDisposable held = UserDataTransactionLock.Acquire(fullTarget);

        int copied = 0;
        int skipped = 0;
        string[] files = Directory.GetFiles(fullSource, "*", SearchOption.AllDirectories);
        foreach (string file in files)
        {
            string rel = Path.GetRelativePath(fullSource, file);
            string target = Path.Combine(fullTarget, rel);

            if (File.Exists(target) && !force)
            {
                skipped++;
                continue;
            }

            try
            {
                SeedFileTransaction.CopyDurableAtomic(
                    file, target, overwrite: force);
                copied++;
            }
            catch (IOException) when (!force && File.Exists(target))
            {
                // A non-Famo writer can still win the create race. Preserve
                // its complete object instead of overwriting it.
                skipped++;
            }
        }

        return new FirstLaunchSeedResult(fullSource, fullTarget, files.Length, copied, skipped);
    }
}
