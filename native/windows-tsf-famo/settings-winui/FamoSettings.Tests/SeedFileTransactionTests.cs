using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using Famo.Settings.Core;
using Xunit;

namespace Famo.Settings.Tests;

[Collection("SeedFileTransaction serial")]
public sealed class SeedFileTransactionTests : IDisposable
{
    private const string TransactionId = "0123456789abcdef0123456789abcdef";
    private readonly string _root = Path.Combine(Path.GetTempPath(),
        $"famo-seed-tests-{Guid.NewGuid():N}");
    private string FamoDir => Path.Combine(_root, "Famo");
    private string InstalledDir => Path.Combine(_root, "installed");
    private string TransactionDir => Path.Combine(
        FamoDir, ".transactions", TransactionId);
    private string ReceiptPath => Path.Combine(TransactionDir, "receipt.json");
    private string RolledBackReceiptPath => Path.Combine(
        FamoDir, ".transactions", $"{TransactionId}.rolledback.json");

    public SeedFileTransactionTests()
    {
        UserDataTransactionLock.LocalAppDataOverrideForTests =
            Path.Combine(_root, "lock-local-app-data");
    }

    [Fact]
    public void PrepareApplyRollback_IsWriteAheadIdempotentAndRestoresExistingFiles()
    {
        Write(Path.Combine(InstalledDir, "payload.txt"), "payload");
        string settings = Path.Combine(FamoDir, "famo-settings.json");
        Write(settings, "before");

        string hash = Prepare(staged =>
            Write(Path.Combine(staged, "famo-settings.json"), "after"));

        Assert.Equal("before", File.ReadAllText(settings));
        Assert.False(File.Exists(Path.Combine(FamoDir, "payload.txt")));
        Assert.Equal(hash, HashFile(ReceiptPath));

        Assert.True(SeedFileTransaction.ApplyPrepared(
            TransactionId, hash, FamoDir));
        Assert.True(SeedFileTransaction.ApplyPrepared(
            TransactionId, hash, FamoDir));
        Assert.Equal("after", File.ReadAllText(settings));
        Assert.Equal("payload", File.ReadAllText(
            Path.Combine(FamoDir, "payload.txt")));

        Assert.True(SeedFileTransaction.Rollback(
            TransactionId, hash, FamoDir));
        Assert.Equal("before", File.ReadAllText(settings));
        Assert.False(File.Exists(Path.Combine(FamoDir, "payload.txt")));
        Assert.False(Directory.Exists(TransactionDir));
        Assert.True(File.Exists(RolledBackReceiptPath));
        Assert.True(SeedFileTransaction.Rollback(
            TransactionId, hash, FamoDir));
    }

    [Fact]
    public void RollbackCrashAfterTreeDeletionRetriesOnlyWithPinnedTombstone()
    {
        Write(Path.Combine(InstalledDir, "payload.txt"), "payload");
        string hash = Prepare();
        Assert.True(SeedFileTransaction.ApplyPrepared(
            TransactionId, hash, FamoDir));
        SeedFileTransaction.AfterRollbackTransactionDeleteForTests = () =>
            throw new IOException(
                "simulated crash after rollback transaction deletion");

        Assert.Throws<IOException>(() => SeedFileTransaction.Rollback(
            TransactionId, hash, FamoDir));
        SeedFileTransaction.AfterRollbackTransactionDeleteForTests = null;

        Assert.False(Directory.Exists(TransactionDir));
        Assert.True(File.Exists(RolledBackReceiptPath));
        Assert.False(File.Exists(Path.Combine(FamoDir, "payload.txt")));
        Assert.True(SeedFileTransaction.Rollback(
            TransactionId, hash, FamoDir));
        Assert.Throws<InvalidOperationException>(() => Prepare());

        File.Delete(RolledBackReceiptPath);
        Assert.False(SeedFileTransaction.Rollback(
            TransactionId, hash, FamoDir));
    }

    [Fact]
    public void RollbackRejectsAnUnpinnedOrForgedMissingRootTombstone()
    {
        Write(Path.Combine(InstalledDir, "payload.txt"), "payload");
        string hash = Prepare();
        Assert.True(SeedFileTransaction.ApplyPrepared(
            TransactionId, hash, FamoDir));
        Assert.True(SeedFileTransaction.Rollback(
            TransactionId, hash, FamoDir));
        string valid = File.ReadAllText(RolledBackReceiptPath);

        Assert.Throws<InvalidDataException>(() =>
            SeedFileTransaction.Rollback(
                TransactionId,
                new string('A', 64),
                FamoDir));

        File.WriteAllText(
            RolledBackReceiptPath,
            valid.Replace(
                "\"Phase\":\"Prepared\"",
                "\"Phase\":\"Forged\"",
                StringComparison.Ordinal));
        Assert.Throws<InvalidDataException>(() =>
            SeedFileTransaction.Rollback(TransactionId, hash, FamoDir));
    }

    [Fact]
    public void CommitKeepsAppliedFilesAndIsIdempotent()
    {
        Write(Path.Combine(InstalledDir, "payload.txt"), "payload");
        string hash = Prepare();

        Assert.True(SeedFileTransaction.ApplyPrepared(
            TransactionId, hash, FamoDir));
        Assert.True(SeedFileTransaction.Commit(
            TransactionId, hash, FamoDir));
        Assert.True(SeedFileTransaction.Commit(
            TransactionId, hash, FamoDir));
        Assert.Equal("payload", File.ReadAllText(
            Path.Combine(FamoDir, "payload.txt")));
        Assert.False(Directory.Exists(TransactionDir));
    }

    [Fact]
    public void PrepareRefusesToRebackupAnUnresolvedTransaction()
    {
        Write(Path.Combine(InstalledDir, "payload.txt"), "payload");
        string hash = Prepare();

        InvalidOperationException error = Assert.Throws<InvalidOperationException>(
            () => Prepare());

        Assert.Contains("already exists", error.Message);
        Assert.Equal(hash, HashFile(ReceiptPath));
    }

    [Fact]
    public void DurableReceiptWriteFailureLeavesLiveFilesUntouchedAndRecoverable()
    {
        string live = Path.Combine(FamoDir, "famo-settings.json");
        Write(live, "before");
        Write(Path.Combine(InstalledDir, "payload.txt"), "payload");
        SeedFileTransaction.BeforeDurableArtifactWriteForTests = path =>
        {
            if (string.Equals(path, ReceiptPath,
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new IOException("simulated durable receipt write failure");
            }
        };
        try
        {
            Assert.Throws<IOException>(() => Prepare(staged =>
                Write(Path.Combine(staged, "famo-settings.json"), "after")));
        }
        finally
        {
            SeedFileTransaction.BeforeDurableArtifactWriteForTests = null;
        }

        Assert.Equal("before", File.ReadAllText(live));
        Assert.False(File.Exists(Path.Combine(FamoDir, "payload.txt")));
        Assert.True(Directory.Exists(TransactionDir));
        Assert.False(File.Exists(ReceiptPath));
        Assert.True(SeedFileTransaction.DiscardPrepared(
            TransactionId, FamoDir));
    }

    [Fact]
    public void ReceiptPublishesTheExactPinnedTemporaryObject()
    {
        Write(Path.Combine(InstalledDir, "payload.txt"), "payload");
        string? temporary = null;
        bool replacementBlocked = false;
        SeedFileTransaction.BeforePinnedAtomicFilePublishForTests =
            (destination, exactTemporary) =>
            {
                if (!string.Equals(
                        destination,
                        ReceiptPath,
                        StringComparison.OrdinalIgnoreCase))
                {
                    return;
                }
                temporary = exactTemporary;
                try
                {
                    File.Delete(exactTemporary);
                    File.WriteAllText(exactTemporary, "{}");
                }
                catch (Exception ex) when (
                    ex is IOException or UnauthorizedAccessException)
                {
                    replacementBlocked = true;
                }
            };

        string hash = Prepare();

        Assert.NotNull(temporary);
        Assert.True(replacementBlocked);
        Assert.Equal(hash, HashFile(ReceiptPath));
        Assert.False(File.Exists(temporary));
    }

    [Theory]
    [InlineData("staged")]
    [InlineData("backups")]
    [InlineData("receipt.json")]
    public void DurableArtifactFlushFailureNeverMutatesLiveFiles(
        string artifact)
    {
        string live = Path.Combine(FamoDir, "famo-settings.json");
        Write(live, "before");
        Write(Path.Combine(InstalledDir, "payload.txt"), "payload");
        SeedFileTransaction.BeforeDurableArtifactFlushForTests = path =>
        {
            if (path.Contains(
                Path.DirectorySeparatorChar + artifact,
                StringComparison.OrdinalIgnoreCase))
            {
                throw new IOException(
                    $"simulated durable {artifact} flush failure");
            }
        };
        try
        {
            Assert.Throws<IOException>(() => Prepare(staged =>
                Write(Path.Combine(staged, "famo-settings.json"), "after")));
        }
        finally
        {
            SeedFileTransaction.BeforeDurableArtifactFlushForTests = null;
        }

        Assert.Equal("before", File.ReadAllText(live));
        Assert.False(File.Exists(Path.Combine(FamoDir, "payload.txt")));
        Assert.True(Directory.Exists(TransactionDir));
        Assert.True(SeedFileTransaction.DiscardPrepared(
            TransactionId, FamoDir));
    }

    [Fact]
    public void PrepareRejectsInitialSnapshotAbaMutation()
    {
        string live = Path.Combine(FamoDir, "famo-settings.json");
        Write(live, "A");
        SeedFileTransaction.AfterInitialSnapshotHashForTests = path =>
        {
            if (string.Equals(path, live, StringComparison.OrdinalIgnoreCase))
            {
                OverwriteInPlace(live, "B");
            }
        };
        SeedFileTransaction.AfterInitialSnapshotCopyForTests = path =>
        {
            if (string.Equals(path, live, StringComparison.OrdinalIgnoreCase))
            {
                OverwriteInPlace(live, "A");
            }
        };
        try
        {
            Assert.Throws<IOException>(() => Prepare());
        }
        finally
        {
            SeedFileTransaction.AfterInitialSnapshotHashForTests = null;
            SeedFileTransaction.AfterInitialSnapshotCopyForTests = null;
        }

        Assert.Equal("A", File.ReadAllText(live));
        Assert.False(File.Exists(ReceiptPath));
        Assert.True(SeedFileTransaction.DiscardPrepared(
            TransactionId, FamoDir));
    }

    [Fact]
    public void ApplyRecoversAfterAConflictPartiallyAppliedEarlierEntries()
    {
        Write(Path.Combine(InstalledDir, "a.txt"), "A");
        Write(Path.Combine(InstalledDir, "z.txt"), "Z");
        string hash = Prepare();
        string conflict = Path.Combine(FamoDir, "z.txt");
        Write(conflict, "manual");

        Assert.False(SeedFileTransaction.ApplyPrepared(
            TransactionId, hash, FamoDir));
        Assert.Equal("A", File.ReadAllText(Path.Combine(FamoDir, "a.txt")));
        Assert.Equal("manual", File.ReadAllText(conflict));

        File.Delete(conflict);
        Assert.True(SeedFileTransaction.ApplyPrepared(
            TransactionId, hash, FamoDir));
        Assert.Equal("A", File.ReadAllText(Path.Combine(FamoDir, "a.txt")));
        Assert.Equal("Z", File.ReadAllText(conflict));
    }

    [Fact]
    public void ApplyDoesNotOverwriteANewFileThatAppearedAfterPrepare()
    {
        Write(Path.Combine(InstalledDir, "new.txt"), "seed");
        string hash = Prepare();
        string live = Path.Combine(FamoDir, "new.txt");
        Write(live, "manual");

        Assert.False(SeedFileTransaction.ApplyPrepared(
            TransactionId, hash, FamoDir));
        Assert.Equal("manual", File.ReadAllText(live));
        Assert.True(SeedFileTransaction.Rollback(
            TransactionId, hash, FamoDir));
        Assert.Equal("manual", File.ReadAllText(live));
    }

    [Fact]
    public void ApplyDoesNotClaimAnIdenticalNewFileCreatedByAnotherWriter()
    {
        Write(Path.Combine(InstalledDir, "new.txt"), "seed");
        string hash = Prepare();
        string live = Path.Combine(FamoDir, "new.txt");
        Write(live, "seed");

        Assert.False(SeedFileTransaction.ApplyPrepared(
            TransactionId, hash, FamoDir));
        Assert.True(SeedFileTransaction.Rollback(
            TransactionId, hash, FamoDir));
        Assert.Equal("seed", File.ReadAllText(live));
    }

    [Fact]
    public void ApplyDoesNotClaimAnIdenticalExistingFileEditByAnotherWriter()
    {
        string live = Path.Combine(FamoDir, "famo-settings.json");
        Write(live, "before");
        string hash = Prepare(staged =>
            Write(Path.Combine(staged, "famo-settings.json"), "after"));
        File.WriteAllText(live, "after");

        Assert.False(SeedFileTransaction.ApplyPrepared(
            TransactionId, hash, FamoDir));
        Assert.True(SeedFileTransaction.Rollback(
            TransactionId, hash, FamoDir));
        Assert.Equal("after", File.ReadAllText(live));
    }

    [Fact]
    public void ExistingFileRaceIsDetectedFromTheActuallyDisplacedObject()
    {
        string live = Path.Combine(FamoDir, "famo-settings.json");
        Write(live, "before");
        string hash = Prepare(staged =>
            Write(Path.Combine(staged, "famo-settings.json"), "after"));
        SeedFileTransaction.BeforeAtomicMutationForTests = path =>
        {
            if (string.Equals(path, live, StringComparison.OrdinalIgnoreCase))
            {
                SeedFileTransaction.BeforeAtomicMutationForTests = null;
                OverwriteInPlace(path, "external-edit");
            }
        };
        try
        {
            Assert.False(SeedFileTransaction.ApplyPrepared(
                TransactionId, hash, FamoDir));
        }
        finally
        {
            SeedFileTransaction.BeforeAtomicMutationForTests = null;
        }

        Assert.Equal("external-edit", File.ReadAllText(live));
        Assert.Empty(Directory.GetFiles(FamoDir, "*.famo-displaced-*"));
        Assert.True(Directory.Exists(TransactionDir));
    }

    [Fact]
    public void ApplyResumesAfterCrashImmediatelyAfterDisplacement()
    {
        string live = Path.Combine(FamoDir, "famo-settings.json");
        Write(live, "before");
        string hash = Prepare(staged =>
            Write(Path.Combine(staged, "famo-settings.json"), "after"));
        SeedFileTransaction.AfterAtomicDisplacementForTests = _ =>
            throw new IOException("simulated crash after displacement");
        try
        {
            Assert.Throws<IOException>(() =>
                SeedFileTransaction.ApplyPrepared(
                    TransactionId, hash, FamoDir));
        }
        finally
        {
            SeedFileTransaction.AfterAtomicDisplacementForTests = null;
        }

        Assert.False(File.Exists(live));
        Assert.True(File.Exists(Path.Combine(
            TransactionDir, "quarantine", "00000000.apply.displaced")));
        Assert.False(SeedFileTransaction.Commit(
            TransactionId, hash, FamoDir));
        Assert.True(SeedFileTransaction.ApplyPrepared(
            TransactionId, hash, FamoDir));
        Assert.Equal("after", File.ReadAllText(live));
        Assert.Equal(
            ["00000000.apply.owned"],
            Directory.EnumerateFileSystemEntries(
                    Path.Combine(TransactionDir, "quarantine"))
                .Select(Path.GetFileName));
    }

    [Fact]
    public void ApplyResumesAfterCrashAfterInstallingTheNewObject()
    {
        string live = Path.Combine(FamoDir, "famo-settings.json");
        Write(live, "before");
        string hash = Prepare(staged =>
            Write(Path.Combine(staged, "famo-settings.json"), "after"));
        SeedFileTransaction.AfterAtomicInstallForTests = _ =>
            throw new IOException("simulated crash after install");
        try
        {
            Assert.Throws<IOException>(() =>
                SeedFileTransaction.ApplyPrepared(
                    TransactionId, hash, FamoDir));
        }
        finally
        {
            SeedFileTransaction.AfterAtomicInstallForTests = null;
        }

        Assert.Equal("after", File.ReadAllText(live));
        Assert.True(File.Exists(Path.Combine(
            TransactionDir, "quarantine", "00000000.apply.displaced")));
        Assert.True(SeedFileTransaction.ApplyPrepared(
            TransactionId, hash, FamoDir));
        Assert.Equal("after", File.ReadAllText(live));
        Assert.Equal(
            ["00000000.apply.owned"],
            Directory.EnumerateFileSystemEntries(
                    Path.Combine(TransactionDir, "quarantine"))
                .Select(Path.GetFileName));
    }

    [Fact]
    public void TamperedResumedApplyDisplacementNeverReachesLive()
    {
        string live = Path.Combine(FamoDir, "famo-settings.json");
        Write(live, "before");
        string hash = Prepare(staged =>
            Write(Path.Combine(staged, "famo-settings.json"), "after"));
        SeedFileTransaction.AfterAtomicDisplacementForTests = _ =>
            throw new IOException("simulated apply crash");
        try
        {
            Assert.Throws<IOException>(() =>
                SeedFileTransaction.ApplyPrepared(
                    TransactionId, hash, FamoDir));
        }
        finally
        {
            SeedFileTransaction.AfterAtomicDisplacementForTests = null;
        }
        string displaced = Path.Combine(
            TransactionDir, "quarantine", "00000000.apply.displaced");
        File.WriteAllText(displaced, "attacker");

        Assert.False(SeedFileTransaction.ApplyPrepared(
            TransactionId, hash, FamoDir));
        Assert.False(File.Exists(live));
        Assert.True(SeedFileTransaction.Rollback(
            TransactionId, hash, FamoDir));
        Assert.Equal("before", File.ReadAllText(live));
        Assert.DoesNotContain("attacker", File.ReadAllText(live));
    }

    [Fact]
    public void RollbackDoesNotDeleteANewFileEditedAtTheAtomicDeleteBoundary()
    {
        Write(Path.Combine(InstalledDir, "new.txt"), "seed");
        string hash = Prepare();
        string live = Path.Combine(FamoDir, "new.txt");
        Assert.True(SeedFileTransaction.ApplyPrepared(
            TransactionId, hash, FamoDir));
        SeedFileTransaction.BeforeAtomicMutationForTests = path =>
        {
            if (string.Equals(path, live, StringComparison.OrdinalIgnoreCase))
            {
                SeedFileTransaction.BeforeAtomicMutationForTests = null;
                OverwriteInPlace(path, "external-edit");
            }
        };
        try
        {
            Assert.True(SeedFileTransaction.Rollback(
                TransactionId, hash, FamoDir));
        }
        finally
        {
            SeedFileTransaction.BeforeAtomicMutationForTests = null;
        }

        Assert.Equal("external-edit", File.ReadAllText(live));
        Assert.Empty(Directory.GetFiles(FamoDir, "*.famo-displaced-*"));
    }

    [Fact]
    public void NewFileRollbackResumesAfterCrashAfterAtomicDisplacement()
    {
        string live = Path.Combine(FamoDir, "new.txt");
        Write(Path.Combine(InstalledDir, "new.txt"), "seed");
        string hash = Prepare();
        Assert.True(SeedFileTransaction.ApplyPrepared(
            TransactionId, hash, FamoDir));
        SeedFileTransaction.AfterAtomicDisplacementForTests = _ =>
            throw new IOException("simulated rollback crash");
        try
        {
            Assert.Throws<IOException>(() =>
                SeedFileTransaction.Rollback(
                    TransactionId, hash, FamoDir));
        }
        finally
        {
            SeedFileTransaction.AfterAtomicDisplacementForTests = null;
        }

        Assert.False(File.Exists(live));
        Assert.True(File.Exists(Path.Combine(
            TransactionDir, "quarantine",
            "00000000.rollback-delete.displaced")));
        Assert.True(SeedFileTransaction.Rollback(
            TransactionId, hash, FamoDir));
        Assert.False(File.Exists(live));
        Assert.False(Directory.Exists(TransactionDir));
    }

    [Fact]
    public void ExistingFileRollbackResumesAfterCrashAfterAtomicDisplacement()
    {
        string live = Path.Combine(FamoDir, "famo-settings.json");
        Write(live, "before");
        string hash = Prepare(staged =>
            Write(Path.Combine(staged, "famo-settings.json"), "after"));
        Assert.True(SeedFileTransaction.ApplyPrepared(
            TransactionId, hash, FamoDir));
        SeedFileTransaction.AfterAtomicDisplacementForTests = _ =>
            throw new IOException("simulated rollback crash");
        try
        {
            Assert.Throws<IOException>(() =>
                SeedFileTransaction.Rollback(
                    TransactionId, hash, FamoDir));
        }
        finally
        {
            SeedFileTransaction.AfterAtomicDisplacementForTests = null;
        }

        Assert.False(File.Exists(live));
        Assert.True(File.Exists(Path.Combine(
            TransactionDir, "quarantine",
            "00000000.rollback.displaced")));
        Assert.True(SeedFileTransaction.Rollback(
            TransactionId, hash, FamoDir));
        Assert.Equal("before", File.ReadAllText(live));
        Assert.False(Directory.Exists(TransactionDir));
    }

    [Fact]
    public void ExistingFileRollbackResumesAfterCrashAndPreservesNewLiveObject()
    {
        string live = Path.Combine(FamoDir, "famo-settings.json");
        Write(live, "before");
        string hash = Prepare(staged =>
            Write(Path.Combine(staged, "famo-settings.json"), "after"));
        Assert.True(SeedFileTransaction.ApplyPrepared(
            TransactionId, hash, FamoDir));
        SeedFileTransaction.AfterAtomicDisplacementForTests = _ =>
            throw new IOException("simulated rollback crash");
        try
        {
            Assert.Throws<IOException>(() =>
                SeedFileTransaction.Rollback(
                    TransactionId, hash, FamoDir));
        }
        finally
        {
            SeedFileTransaction.AfterAtomicDisplacementForTests = null;
        }

        Assert.False(File.Exists(live));
        Write(live, "later-user-object");

        Assert.True(SeedFileTransaction.Rollback(
            TransactionId, hash, FamoDir));
        Assert.Equal("later-user-object", File.ReadAllText(live));
        Assert.False(Directory.Exists(TransactionDir));
    }

    [Fact]
    public void TamperedResumedExistingRollbackDisplacementNeverReachesLive()
    {
        string live = Path.Combine(FamoDir, "famo-settings.json");
        Write(live, "before");
        string hash = Prepare(staged =>
            Write(Path.Combine(staged, "famo-settings.json"), "after"));
        Assert.True(SeedFileTransaction.ApplyPrepared(
            TransactionId, hash, FamoDir));
        SeedFileTransaction.AfterAtomicDisplacementForTests = _ =>
            throw new IOException("simulated rollback crash");
        try
        {
            Assert.Throws<IOException>(() =>
                SeedFileTransaction.Rollback(
                    TransactionId, hash, FamoDir));
        }
        finally
        {
            SeedFileTransaction.AfterAtomicDisplacementForTests = null;
        }
        File.WriteAllText(Path.Combine(
            TransactionDir, "quarantine",
            "00000000.rollback.displaced"), "attacker");

        Assert.True(SeedFileTransaction.Rollback(
            TransactionId, hash, FamoDir));
        Assert.Equal("before", File.ReadAllText(live));
        Assert.DoesNotContain("attacker", File.ReadAllText(live));
    }

    [Fact]
    public void TamperedResumedNewFileRollbackDisplacementNeverReachesLive()
    {
        string live = Path.Combine(FamoDir, "new.txt");
        Write(Path.Combine(InstalledDir, "new.txt"), "seed");
        string hash = Prepare();
        Assert.True(SeedFileTransaction.ApplyPrepared(
            TransactionId, hash, FamoDir));
        SeedFileTransaction.AfterAtomicDisplacementForTests = _ =>
            throw new IOException("simulated rollback delete crash");
        try
        {
            Assert.Throws<IOException>(() =>
                SeedFileTransaction.Rollback(
                    TransactionId, hash, FamoDir));
        }
        finally
        {
            SeedFileTransaction.AfterAtomicDisplacementForTests = null;
        }
        File.WriteAllText(Path.Combine(
            TransactionDir, "quarantine",
            "00000000.rollback-delete.displaced"), "attacker");

        Assert.True(SeedFileTransaction.Rollback(
            TransactionId, hash, FamoDir));
        Assert.False(File.Exists(live));
    }

    [Fact]
    public void RollbackPreservesLaterUserChangesToExistingAndNewFiles()
    {
        string existing = Path.Combine(FamoDir, "famo-settings.json");
        string added = Path.Combine(FamoDir, "payload.txt");
        Write(existing, "before");
        Write(Path.Combine(InstalledDir, "payload.txt"), "payload");
        string hash = Prepare(staged =>
            Write(Path.Combine(staged, "famo-settings.json"), "after"));
        Assert.True(SeedFileTransaction.ApplyPrepared(
            TransactionId, hash, FamoDir));

        File.WriteAllText(existing, "later-existing");
        File.WriteAllText(added, "later-new");

        Assert.True(SeedFileTransaction.Rollback(
            TransactionId, hash, FamoDir));
        Assert.Equal("later-existing", File.ReadAllText(existing));
        Assert.Equal("later-new", File.ReadAllText(added));
        Assert.False(Directory.Exists(TransactionDir));
    }

    [Fact]
    public void ReceiptHashPinsTheExactBytesReadForApply()
    {
        Write(Path.Combine(InstalledDir, "payload.txt"), "payload");
        string hash = Prepare();
        File.AppendAllText(ReceiptPath, " ");

        Assert.Throws<InvalidDataException>(() =>
            SeedFileTransaction.ApplyPrepared(TransactionId, hash, FamoDir));
        Assert.False(File.Exists(Path.Combine(FamoDir, "payload.txt")));
    }

    [Fact]
    public void StagedPayloadTamperIsRejectedBeforeLiveCreation()
    {
        Write(Path.Combine(InstalledDir, "payload.txt"), "payload");
        string hash = Prepare();
        File.WriteAllText(
            Path.Combine(TransactionDir, "staged", "payload.txt"), "tampered");

        Assert.Throws<InvalidDataException>(() =>
            SeedFileTransaction.ApplyPrepared(TransactionId, hash, FamoDir));
        Assert.False(File.Exists(Path.Combine(FamoDir, "payload.txt")));
    }

    [Fact]
    public void BackupTamperIsRejectedWithoutDeletingTheRecoveryReceipt()
    {
        string live = Path.Combine(FamoDir, "famo-settings.json");
        Write(live, "before");
        string hash = Prepare(staged =>
            Write(Path.Combine(staged, "famo-settings.json"), "after"));
        Assert.True(SeedFileTransaction.ApplyPrepared(
            TransactionId, hash, FamoDir));
        File.WriteAllText(
            Path.Combine(TransactionDir, "backups", "00000000.bin"),
            "tampered");

        Assert.Throws<InvalidDataException>(() =>
            SeedFileTransaction.Rollback(TransactionId, hash, FamoDir));
        Assert.Equal("after", File.ReadAllText(live));
        Assert.True(File.Exists(ReceiptPath));
    }

    [Theory]
    [InlineData("relative-traversal")]
    [InlineData("backup-traversal")]
    [InlineData("case-alias")]
    public void SemanticallyInvalidReceiptIsRejectedEvenWhenItsNewHashIsPinned(
        string mutation)
    {
        Write(Path.Combine(InstalledDir, "a.txt"), "A");
        Write(Path.Combine(InstalledDir, "b.txt"), "B");
        if (mutation == "backup-traversal")
        {
            Write(Path.Combine(FamoDir, "a.txt"), "before");
        }
        _ = Prepare(staged =>
        {
            if (mutation == "backup-traversal")
            {
                Write(Path.Combine(staged, "a.txt"), "after");
            }
        });
        JsonObject receipt = ReadReceipt();
        JsonArray entries = receipt["Entries"]!.AsArray();
        if (mutation == "relative-traversal")
        {
            entries[0]!["RelativePath"] = "../escape.txt";
        }
        else if (mutation == "backup-traversal")
        {
            JsonNode existing = entries.Single(entry =>
                entry!["Existed"]!.GetValue<bool>())!;
            existing["BackupName"] = "../outside.bin";
        }
        else
        {
            entries[1]!["RelativePath"] =
                entries[0]!["RelativePath"]!.GetValue<string>().ToUpperInvariant();
        }
        receipt["Digest"] = ComputeReceiptDigest(receipt);
        File.WriteAllText(ReceiptPath, receipt.ToJsonString());
        string repinnedHash = HashFile(ReceiptPath);

        Assert.ThrowsAny<Exception>(() =>
            SeedFileTransaction.ApplyPrepared(
                TransactionId, repinnedHash, FamoDir));
    }

    [Fact]
    public void ReparsePointInInstalledPayloadIsRejectedWhenPlatformAllowsIt()
    {
        Directory.CreateDirectory(InstalledDir);
        string outside = Path.Combine(_root, "outside");
        Directory.CreateDirectory(outside);
        Write(Path.Combine(outside, "payload.txt"), "payload");
        try
        {
            Directory.CreateSymbolicLink(
                Path.Combine(InstalledDir, "linked"), outside);
        }
        catch (Exception ex) when (
            ex is UnauthorizedAccessException or IOException or
            PlatformNotSupportedException)
        {
            return;
        }

        Assert.Throws<IOException>(() => Prepare());
        Assert.False(File.Exists(Path.Combine(FamoDir, "linked", "payload.txt")));
    }

    [Fact]
    public void PreexistingTransactionParentJunctionCannotCreateOutsideTheRoot()
    {
        Directory.CreateDirectory(FamoDir);
        string outside = Path.Combine(_root, "outside-transactions");
        Directory.CreateDirectory(outside);
        try
        {
            Directory.CreateSymbolicLink(
                Path.Combine(FamoDir, ".transactions"), outside);
        }
        catch (Exception ex) when (
            ex is UnauthorizedAccessException or IOException or
            PlatformNotSupportedException)
        {
            return;
        }
        Write(Path.Combine(InstalledDir, "payload.txt"), "payload");

        Assert.Throws<IOException>(() => Prepare());
        Assert.Empty(Directory.EnumerateFileSystemEntries(outside));
        Assert.False(Directory.Exists(Path.Combine(outside, TransactionId)));
    }

    [Fact]
    public async Task NormalSettingsWriterWaitsForTheTransactionMutex()
    {
        string live = Path.Combine(FamoDir, "famo-settings.json");
        Write(live, "before");
        string hash = Prepare(staged =>
            Write(Path.Combine(staged, "famo-settings.json"), "after"));
        using var started = new ManualResetEventSlim();
        Task? writer = null;
        SeedFileTransaction.BeforeAtomicMutationForTests = path =>
        {
            if (!string.Equals(path, live, StringComparison.OrdinalIgnoreCase))
            {
                return;
            }
            SeedFileTransaction.BeforeAtomicMutationForTests = null;
            writer = Task.Run(() =>
            {
                started.Set();
                SafeJsonFile.WriteAtomic(live, "normal-writer");
            });
            Assert.True(started.Wait(TimeSpan.FromSeconds(2)));
            Thread.Sleep(100);
            Assert.False(writer.IsCompleted);
        };
        try
        {
            Assert.True(SeedFileTransaction.ApplyPrepared(
                TransactionId, hash, FamoDir));
        }
        finally
        {
            SeedFileTransaction.BeforeAtomicMutationForTests = null;
        }

        Assert.NotNull(writer);
        await writer!.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal("normal-writer", File.ReadAllText(live));
        Assert.True(SeedFileTransaction.Rollback(
            TransactionId, hash, FamoDir));
        Assert.Equal("normal-writer", File.ReadAllText(live));
    }

    [Theory]
    [InlineData("regular")]
    [InlineData("reparse")]
    public void SafeJsonWriterIgnoresForeignLegacyTempAndPublishesPinnedUniqueTemps(
        string legacyTempKind)
    {
        string live = Path.Combine(FamoDir, "famo-settings.json");
        string legacyTemp = live + ".tmp";
        string outside = Path.Combine(_root, "outside-sentinel.json");
        Write(live, "before");
        Write(outside, "outside");
        if (legacyTempKind == "reparse")
        {
            try
            {
                File.CreateSymbolicLink(legacyTemp, outside);
            }
            catch (Exception ex) when (
                ex is UnauthorizedAccessException or IOException or
                PlatformNotSupportedException)
            {
                return;
            }
        }
        else
        {
            Write(legacyTemp, "foreign");
        }

        var exactTemporaries = new List<string>();
        int flushCallbacks = 0;
        int blockedReplacements = 0;
        SeedFileTransaction.BeforeDurableArtifactFlushForTests = destination =>
        {
            if (string.Equals(
                    destination, live, StringComparison.OrdinalIgnoreCase))
            {
                flushCallbacks++;
            }
        };
        SeedFileTransaction.BeforePinnedAtomicFilePublishForTests =
            (destination, exactTemporary) =>
            {
                if (!string.Equals(
                        destination, live, StringComparison.OrdinalIgnoreCase))
                {
                    return;
                }
                Assert.Equal(exactTemporaries.Count + 1, flushCallbacks);
                Assert.StartsWith(
                    live + ".famo-tmp-",
                    exactTemporary,
                    StringComparison.OrdinalIgnoreCase);
                Assert.False(string.Equals(
                    legacyTemp,
                    exactTemporary,
                    StringComparison.OrdinalIgnoreCase));
                exactTemporaries.Add(exactTemporary);
                try
                {
                    File.Delete(exactTemporary);
                    File.WriteAllText(exactTemporary, "foreign");
                }
                catch (Exception ex) when (
                    ex is IOException or UnauthorizedAccessException)
                {
                    blockedReplacements++;
                }
            };

        SafeJsonFile.WriteAtomic(live, "after-1");
        SafeJsonFile.WriteAtomic(live, "after-2");

        Assert.Equal("after-2", File.ReadAllText(live));
        Assert.Equal(2, flushCallbacks);
        Assert.Equal(2, blockedReplacements);
        Assert.Equal(2, exactTemporaries.Distinct(
            StringComparer.OrdinalIgnoreCase).Count());
        Assert.All(exactTemporaries, temporary =>
            Assert.False(File.Exists(temporary)));
        Assert.Equal("outside", File.ReadAllText(outside));
        Assert.True(File.Exists(legacyTemp));
        Assert.Equal(
            legacyTempKind == "regular" ? "foreign" : "outside",
            File.ReadAllText(legacyTemp));
        Assert.Empty(Directory.GetFiles(
            FamoDir,
            "famo-settings.json.famo-tmp-*",
            SearchOption.TopDirectoryOnly));
    }

    [Theory]
    [InlineData("before", false)]
    [InlineData("displaced", false)]
    [InlineData("installed", false)]
    [InlineData("before", true)]
    [InlineData("displaced", true)]
    [InlineData("installed", true)]
    public void ExistingFileParentCannotBecomeAJunctionDuringApplyOrRollback(
        string hook, bool rollback)
    {
        if (!CanCreateDirectorySymlink())
        {
            return;
        }
        string relative = Path.Combine("nested", "payload.txt");
        string live = Path.Combine(FamoDir, relative);
        Write(live, "before");
        Write(Path.Combine(InstalledDir, relative), "after");
        string hash = Prepare(staged =>
            File.WriteAllText(Path.Combine(staged, relative), "after"));
        if (rollback)
        {
            Assert.True(SeedFileTransaction.ApplyPrepared(
                TransactionId, hash, FamoDir));
        }

        ParentSwapProbe probe = InstallParentSwapHook(live, hook);
        bool result = rollback
            ? SeedFileTransaction.Rollback(TransactionId, hash, FamoDir)
            : SeedFileTransaction.ApplyPrepared(
                TransactionId, hash, FamoDir);
        probe.RestoreIfNeeded();

        Assert.True(result);
        Assert.True(probe.Attempted);
        Assert.True(probe.Blocked);
        Assert.False(probe.Swapped);
        Assert.Equal(rollback ? "before" : "after", File.ReadAllText(live));
        Assert.Equal(
            ["sentinel.txt"],
            Directory.EnumerateFileSystemEntries(probe.Outside)
                .Select(Path.GetFileName)
                .Order(StringComparer.OrdinalIgnoreCase));
    }

    [Theory]
    [InlineData("before", false)]
    [InlineData("installed", false)]
    [InlineData("before", true)]
    [InlineData("displaced", true)]
    public void NewFileParentCannotBecomeAJunctionDuringApplyOrRollbackDelete(
        string hook, bool rollback)
    {
        if (!CanCreateDirectorySymlink())
        {
            return;
        }
        string relative = Path.Combine("nested", "new.txt");
        Write(Path.Combine(InstalledDir, relative), "seed");
        string hash = Prepare();
        string live = Path.Combine(FamoDir, relative);
        if (rollback)
        {
            Assert.True(SeedFileTransaction.ApplyPrepared(
                TransactionId, hash, FamoDir));
        }

        ParentSwapProbe probe = InstallParentSwapHook(live, hook);
        bool result = rollback
            ? SeedFileTransaction.Rollback(TransactionId, hash, FamoDir)
            : SeedFileTransaction.ApplyPrepared(
                TransactionId, hash, FamoDir);
        probe.RestoreIfNeeded();

        Assert.True(result);
        Assert.True(probe.Attempted);
        Assert.True(probe.Blocked);
        Assert.False(probe.Swapped);
        Assert.Equal(!rollback, File.Exists(live));
        Assert.Equal(
            ["sentinel.txt"],
            Directory.EnumerateFileSystemEntries(probe.Outside)
                .Select(Path.GetFileName)
                .Order(StringComparer.OrdinalIgnoreCase));
    }

    public void Dispose()
    {
        SeedFileTransaction.BeforeAtomicMutationForTests = null;
        SeedFileTransaction.AfterAtomicDisplacementForTests = null;
        SeedFileTransaction.AfterAtomicInstallForTests = null;
        SeedFileTransaction.BeforeDurableArtifactWriteForTests = null;
        SeedFileTransaction.BeforeDurableArtifactFlushForTests = null;
        SeedFileTransaction.BeforeAtomicFilePublishForTests = null;
        SeedFileTransaction.BeforePinnedAtomicFilePublishForTests = null;
        SeedFileTransaction.AfterInitialSnapshotHashForTests = null;
        SeedFileTransaction.AfterInitialSnapshotCopyForTests = null;
        SeedFileTransaction.AfterRollbackTransactionDeleteForTests = null;
        UserDataTransactionLock.LocalAppDataOverrideForTests =
            TestUserDataLockEnvironment.LocalAppDataRoot;
        if (Directory.Exists(_root))
        {
            Directory.Delete(_root, recursive: true);
        }
    }

    private ParentSwapProbe InstallParentSwapHook(
        string live, string hook)
    {
        var probe = new ParentSwapProbe(
            live,
            Path.Combine(_root, $"outside-{Guid.NewGuid():N}"));
        Directory.CreateDirectory(probe.Outside);
        Write(Path.Combine(probe.Outside, "sentinel.txt"), "outside");
        Action<string> callback = path =>
        {
            if (string.Equals(path, live, StringComparison.OrdinalIgnoreCase))
            {
                probe.TrySwap();
            }
        };
        if (hook == "before")
        {
            SeedFileTransaction.BeforeAtomicMutationForTests = callback;
        }
        else if (hook == "displaced")
        {
            SeedFileTransaction.AfterAtomicDisplacementForTests = callback;
        }
        else if (hook == "installed")
        {
            SeedFileTransaction.AfterAtomicInstallForTests = callback;
        }
        else
        {
            throw new ArgumentOutOfRangeException(nameof(hook));
        }
        return probe;
    }

    private bool CanCreateDirectorySymlink()
    {
        string target = Path.Combine(_root, "symlink-target");
        string link = Path.Combine(_root, "symlink-probe");
        Directory.CreateDirectory(target);
        try
        {
            Directory.CreateSymbolicLink(link, target);
            Directory.Delete(link);
            return true;
        }
        catch (Exception ex) when (
            ex is UnauthorizedAccessException or IOException or
            PlatformNotSupportedException)
        {
            return false;
        }
    }

    private sealed class ParentSwapProbe(string live, string outside)
    {
        private readonly string _parent =
            Path.GetDirectoryName(live)
            ?? throw new InvalidOperationException("Live file has no parent.");
        private readonly string _parked =
            (Path.GetDirectoryName(live)
             ?? throw new InvalidOperationException("Live file has no parent.")) +
            $".parked-{Guid.NewGuid():N}";

        internal string Outside { get; } = outside;
        internal bool Attempted { get; private set; }
        internal bool Blocked { get; private set; }
        internal bool Swapped { get; private set; }

        internal void TrySwap()
        {
            Attempted = true;
            try
            {
                Directory.Move(_parent, _parked);
                Swapped = true;
                Directory.CreateSymbolicLink(_parent, Outside);
            }
            catch (Exception ex) when (
                ex is UnauthorizedAccessException or IOException)
            {
                Blocked = true;
            }
        }

        internal void RestoreIfNeeded()
        {
            SeedFileTransaction.BeforeAtomicMutationForTests = null;
            SeedFileTransaction.AfterAtomicDisplacementForTests = null;
            SeedFileTransaction.AfterAtomicInstallForTests = null;
            if (!Swapped)
            {
                return;
            }
            if (Directory.Exists(_parent) &&
                (File.GetAttributes(_parent) &
                 FileAttributes.ReparsePoint) != 0)
            {
                Directory.Delete(_parent);
            }
            if (Directory.Exists(_parked))
            {
                Directory.Move(_parked, _parent);
            }
        }
    }

    private string Prepare(Action<string>? customize = null)
    {
        Directory.CreateDirectory(InstalledDir);
        return SeedFileTransaction.Prepare(
            TransactionId,
            InstalledDir,
            staged =>
            {
                FirstLaunchSeeder.Seed(InstalledDir, staged);
                customize?.Invoke(staged);
            },
            FamoDir);
    }

    private static void Write(string path, string content)
    {
        Directory.CreateDirectory(
            Path.GetDirectoryName(path)
            ?? throw new InvalidOperationException("Test path has no parent."));
        File.WriteAllText(path, content);
    }

    private static void OverwriteInPlace(string path, string content)
    {
        byte[] bytes = System.Text.Encoding.UTF8.GetBytes(content);
        using var stream = new FileStream(
            path, FileMode.Open, FileAccess.Write,
            FileShare.ReadWrite | FileShare.Delete);
        stream.SetLength(0);
        stream.Write(bytes);
        stream.Flush(flushToDisk: true);
    }

    private JsonObject ReadReceipt() =>
        JsonNode.Parse(File.ReadAllText(ReceiptPath))!.AsObject();

    private static string HashFile(string path)
    {
        using FileStream stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream));
    }

    private static string ComputeReceiptDigest(JsonObject receipt)
    {
        var canonical = new StringBuilder();
        Append("schema", receipt["Schema"]!.GetValue<string>());
        Append("transaction", receipt["TransactionId"]!.GetValue<string>());
        Append("phase", receipt["Phase"]!.GetValue<string>());
        foreach (JsonNode? item in receipt["Entries"]!.AsArray())
        {
            JsonObject entry = item!.AsObject();
            Append("path", entry["RelativePath"]!.GetValue<string>());
            Append("existed",
                entry["Existed"]!.GetValue<bool>() ? "1" : "0");
            Append("before", entry["BeforeHash"]!.GetValue<string>());
            Append("backup_name", entry["BackupName"]!.GetValue<string>());
            Append("backup_hash", entry["BackupHash"]!.GetValue<string>());
            Append("after", entry["AfterHash"]!.GetValue<string>());
        }
        return Convert.ToHexString(SHA256.HashData(
            Encoding.UTF8.GetBytes(canonical.ToString())));

        void Append(string name, string value) =>
            canonical.Append(name).Append(':').Append(value.Length).Append(':')
                .Append(value).Append(';');
    }
}

[CollectionDefinition("SeedFileTransaction serial", DisableParallelization = true)]
public sealed class SeedFileTransactionSerialCollection;
