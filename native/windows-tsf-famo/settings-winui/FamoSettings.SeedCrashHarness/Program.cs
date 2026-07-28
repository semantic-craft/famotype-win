using Famo.Settings.Core;

if (args.Length is not 3 and not 5)
{
    return 2;
}

bool extendedMode = args.Length == 5;
bool crashAfterDeleteDispositionClear = extendedMode &&
    string.Equals(
        args[0],
        "after-clear-before-rename",
        StringComparison.Ordinal);
bool crashAfterRenameBeforeRecoveryCleanup = extendedMode &&
    string.Equals(
        args[0],
        "after-rename-before-recovery-cleanup",
        StringComparison.Ordinal);
bool crashBeforePublish = !extendedMode ||
    string.Equals(
        args[0],
        "before-publish",
        StringComparison.Ordinal);
if (!crashAfterDeleteDispositionClear &&
    !crashAfterRenameBeforeRecoveryCleanup &&
    !crashBeforePublish)
{
    return 2;
}
int pathOffset = extendedMode ? 1 : 0;
string source = Path.GetFullPath(args[pathOffset]);
string destination = Path.GetFullPath(args[pathOffset + 1]);
string marker = Path.GetFullPath(args[pathOffset + 2]);
if (extendedMode)
{
    UserDataTransactionLock.LocalAppDataOverrideForTests =
        Path.GetFullPath(args[pathOffset + 3]);
}

string? exactTemporary = null;
SeedFileTransaction.BeforePinnedAtomicFilePublishForTests =
    (published, temporary) =>
    {
        if (!string.Equals(
                published,
                destination,
                StringComparison.OrdinalIgnoreCase))
        {
            return;
        }
        exactTemporary = temporary;
        if (!crashBeforePublish)
        {
            return;
        }
        WriteMarkerAndCrash(marker, temporary);
    };
SeedFileTransaction.AfterPinnedTemporaryDeleteDispositionClearedForTests =
    () =>
    {
        if (!crashAfterDeleteDispositionClear)
        {
            return;
        }
        WriteMarkerAndCrash(
            marker,
            exactTemporary
            ?? throw new InvalidOperationException(
                "Pinned temporary path was not captured."));
    };
SeedFileTransaction.AfterPinnedAtomicFileRenameForTests =
    () =>
    {
        if (!crashAfterRenameBeforeRecoveryCleanup)
        {
            return;
        }
        WriteMarkerAndCrash(
            marker,
            exactTemporary
            ?? throw new InvalidOperationException(
                "Pinned temporary path was not captured."));
    };

SeedFileTransaction.CopyDurableAtomic(
    source, destination, overwrite: false);
return 3;

static void WriteMarkerAndCrash(string marker, string temporary)
{
        Directory.CreateDirectory(
            Path.GetDirectoryName(marker)
            ?? throw new InvalidOperationException(
                "Crash marker has no parent."));
        using (var output = new FileStream(
                   marker,
                   FileMode.CreateNew,
                   FileAccess.Write,
                   FileShare.Read,
                   bufferSize: 4096,
                   options: FileOptions.WriteThrough))
        using (var writer = new StreamWriter(output))
        {
            writer.Write(temporary);
            writer.Flush();
            output.Flush(flushToDisk: true);
        }
        Environment.FailFast(
            "intentional seed publication crash selfcheck");
}
