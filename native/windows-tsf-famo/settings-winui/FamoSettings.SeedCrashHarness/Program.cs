using Famo.Settings.Core;

if (args.Length != 3)
{
    return 2;
}

string source = Path.GetFullPath(args[0]);
string destination = Path.GetFullPath(args[1]);
string marker = Path.GetFullPath(args[2]);
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
    };

SeedFileTransaction.CopyDurableAtomic(
    source, destination, overwrite: false);
return 3;
