using Xunit;

namespace Famo.Settings.Tests;

public sealed class BoundedIpcPatchContractTests
{
    [Fact]
    public void BoundedIpcPatchCapsConnectReadAndWrite()
    {
        string patch = File.ReadAllText(WeaselForkFile("features", "bounded-ipc-connect.patch"));

        Assert.Contains("kFamoPipeConnectBudgetMs = 1500", patch);
        Assert.Contains("kFamoPipeIoBudgetMs = 1500", patch);
        Assert.Contains("FILE_FLAG_OVERLAPPED", patch);
        Assert.Contains("WaitForSingleObject(overlapped.hEvent, kFamoPipeIoBudgetMs)", patch);
        Assert.Contains("CancelIo(pipe)", patch);
        Assert.Contains("_ThrowCode(ERROR_SEM_TIMEOUT)", patch);
        Assert.Contains("-  ::FlushFileBuffers(pipe);", patch);
        Assert.DoesNotContain("+  ::FlushFileBuffers(pipe);", patch);
    }

    [Fact]
    public void BoundedIpcPatchTreatsMalformedResponsesAsFailOpenProtocolFailures()
    {
        string patch = File.ReadAllText(WeaselForkFile("features", "bounded-ipc-connect.patch"));

        Assert.Contains("ERROR_INVALID_DATA", patch);
        Assert.Contains("lread < rec_len", patch);
        Assert.Contains("body_error == ERROR_MORE_DATA ? ERROR_INVALID_DATA", patch);
        Assert.Contains("catch (DWORD ex)", patch);
        Assert.Contains("ex == ERROR_SEM_TIMEOUT", patch);
        Assert.Contains("ClearBufferStream();", patch);
    }

    [Fact]
    public void BoundedIpcPatchRunsBeforeFeaturePatchesThatAddNewIpcCommands()
    {
        string apply = File.ReadAllText(WeaselForkFile("apply-famo-features.ps1"));

        int bounded = apply.IndexOf("features/bounded-ipc-connect.patch", StringComparison.Ordinal);
        int instant = apply.IndexOf("features/instant-apply.patch", StringComparison.Ordinal);
        int select = apply.IndexOf("features/select-schema.patch", StringComparison.Ordinal);

        Assert.True(bounded >= 0, "bounded IPC patch must remain in the feature chain");
        Assert.True(instant > bounded, "bounded IPC must be applied before instant reload IPC commands");
        Assert.True(select > bounded, "bounded IPC must be applied before select-schema IPC commands");
    }

    private static string WeaselForkFile(params string[] parts)
    {
        string[] path = new[] { "native", "windows-tsf-famo", "weasel-fork" }.Concat(parts).ToArray();
        return RepoFile(path);
    }

    private static string RepoFile(string[] pathParts)
    {
        string? dir = AppContext.BaseDirectory;
        while (!string.IsNullOrEmpty(dir))
        {
            string candidate = Path.Combine(new[] { dir }.Concat(pathParts).ToArray());
            if (File.Exists(candidate))
            {
                return candidate;
            }

            dir = Directory.GetParent(dir)?.FullName;
        }

        throw new FileNotFoundException($"Could not locate {Path.Combine(pathParts)}");
    }
}
