using System.Buffers.Binary;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.Win32.SafeHandles;

namespace Famo.Settings.Core;

/// <summary>
/// Two-step, write-ahead receipt for installer seed writes. Prepare never
/// changes live files; the elevated journal pins the returned receipt hash
/// before ApplyPrepared is allowed to change them.
/// </summary>
public static class SeedFileTransaction
{
    private const string Schema = "famo-seed-transaction-v1";
    private const string ReceiptName = "receipt.json";
    private const int MaxEntries = 10_000;
    private const int MaxReceiptBytes = 4 * 1024 * 1024;
    private const string AtomicPublicationRecoverySchema =
        "famo-atomic-publication-recovery-v1";
    private const int MaxAtomicPublicationRecoveryBytes = 512 * 1024;
    private static readonly byte[] AtomicPublicationRecoveryHeader =
        Encoding.ASCII.GetBytes("FAMO-PUBLISH-RECOVERY-V1");
    private static readonly byte[] AtomicPublicationRecoveryCommit =
        Encoding.ASCII.GetBytes("FAMO-PUBLISH-COMMITTED-V1");
    internal static Action<string>? BeforeAtomicMutationForTests { get; set; }
    internal static Action<string>? AfterAtomicDisplacementForTests { get; set; }
    internal static Action<string>? AfterAtomicInstallForTests { get; set; }
    internal static Action<string>? BeforeDurableArtifactWriteForTests { get; set; }
    internal static Action<string>? BeforeDurableArtifactFlushForTests { get; set; }
    internal static Action<string>? BeforeAtomicFilePublishForTests { get; set; }
    internal static Action<string, string>? BeforePinnedAtomicFilePublishForTests
    {
        get;
        set;
    }
    internal static Action? AfterPinnedTemporaryDeleteDispositionClearedForTests
    {
        get;
        set;
    }
    internal static Action? AfterPinnedAtomicFileRenameForTests
    {
        get;
        set;
    }
    internal static Action<string>? AfterInitialSnapshotHashForTests { get; set; }
    internal static Action<string>? AfterInitialSnapshotCopyForTests { get; set; }
    internal static Action? AfterRollbackTransactionDeleteForTests { get; set; }
    private static readonly string[] GeneratedFiles =
    [
        "famo-settings.json",
        "famo-style.yaml",
        "famo-options.yaml",
        "famo-select-schema.txt",
        "default.custom.yaml",
        "rime_ice.custom.yaml",
        "wubi86_jidian.custom.yaml",
        "wubi86_jidian_pinyin.custom.yaml",
        "wubi86_jidian_trad.custom.yaml",
        "wubi86_jidian_trad_pinyin.custom.yaml",
    ];

    public static string Prepare(
        string transactionId,
        string installedDataDir,
        Action<string> generateStagedFiles,
        string? famoDir = null)
    {
        string root = FullRoot(famoDir);
        using IDisposable held = UserDataTransactionLock.Acquire(root);
        ValidateTransactionId(transactionId);
        ArgumentNullException.ThrowIfNull(generateStagedFiles);
        EnsureDirectoryTree(root, create: true);
        string transactionRoot = TransactionRoot(root, transactionId);
        if (Directory.Exists(transactionRoot) ||
            File.Exists(RolledBackTombstonePath(root, transactionId)) ||
            Directory.Exists(RolledBackTombstonePath(root, transactionId)))
        {
            throw new InvalidOperationException(
                "Seed transaction already exists; recovery must resolve it before prepare.");
        }

        string stagedRoot = Path.Combine(transactionRoot, "staged");
        string backupRoot = Path.Combine(transactionRoot, "backups");
        string quarantineRoot = Path.Combine(transactionRoot, "quarantine");
        EnsureDirectoryTree(stagedRoot, create: true);
        EnsureDirectoryTree(backupRoot, create: true);
        EnsureDirectoryTree(quarantineRoot, create: true);

        SortedSet<string> managed = GetManagedFiles(installedDataDir);
        var initialHashes = new Dictionary<string, string>(
            StringComparer.OrdinalIgnoreCase);
        foreach (string relative in managed)
        {
            string live = SafePath(root, relative, allowMissingLeaf: true);
            if (!File.Exists(live))
            {
                continue;
            }
            EnsureRegularFile(root, live);
            string initialHash = HashFile(live);
            AfterInitialSnapshotHashForTests?.Invoke(live);
            initialHashes.Add(relative, initialHash);
            string stagedSnapshot = SafePath(
                stagedRoot, relative, allowMissingLeaf: true);
            CopyFileAtomic(live, stagedSnapshot);
            AfterInitialSnapshotCopyForTests?.Invoke(live);
            if (!string.Equals(
                    HashFile(stagedSnapshot),
                    initialHash,
                    StringComparison.Ordinal))
            {
                throw new IOException(
                    $"Seed snapshot changed while copying: {relative}");
            }
        }

        generateStagedFiles(stagedRoot);
        string[] stagedFiles = EnumerateRegularFiles(stagedRoot).ToArray();
        foreach (string staged in stagedFiles)
        {
            // The generator is intentionally decoupled from this transaction
            // API. Rewrite every generated object through our write-through
            // path so the journal can never pin merely cached staged bytes.
            CopyFileAtomic(staged, staged);
            managed.Add(NormalizeRelative(Path.GetRelativePath(stagedRoot, staged)));
        }
        if (managed.Count > MaxEntries)
        {
            throw new InvalidDataException("Seed transaction file count is too large.");
        }

        var entries = new List<Entry>();
        foreach (string relative in managed)
        {
            string staged = SafePath(stagedRoot, relative, allowMissingLeaf: false);
            if (!File.Exists(staged))
            {
                continue;
            }
            EnsureRegularFile(stagedRoot, staged);
            string afterHash = HashFile(staged);
            bool existed = initialHashes.TryGetValue(relative, out string? beforeHash);
            if (existed && string.Equals(beforeHash, afterHash,
                    StringComparison.Ordinal))
            {
                // Existing payload files are always skip-if-present. Because
                // beforeHash is from the first snapshot (not a later reread),
                // a concurrent live edit can never be copied back over it.
                continue;
            }

            string backupName = "";
            string backupHash = "";
            if (existed)
            {
                backupName = $"{entries.Count:D8}.bin";
                string backup = Path.Combine(backupRoot, backupName);
                CopyFileAtomic(SafePath(root, relative, allowMissingLeaf: false),
                    backup);
                backupHash = HashFile(backup);
                if (!string.Equals(backupHash, beforeHash,
                        StringComparison.Ordinal))
                {
                    throw new IOException($"Seed snapshot changed while backing up: {relative}");
                }
            }
            entries.Add(new Entry
            {
                RelativePath = relative,
                Existed = existed,
                BeforeHash = beforeHash ?? "",
                BackupName = backupName,
                BackupHash = backupHash,
                AfterHash = afterHash,
            });
        }

        var receipt = new Receipt
        {
            Schema = Schema,
            TransactionId = transactionId,
            Phase = "Prepared",
            Entries = entries,
        };
        string receiptPath = Path.Combine(transactionRoot, ReceiptName);
        WriteReceipt(receiptPath, receipt);
        return HashReceiptFile(receiptPath);
    }

    public static bool ApplyPrepared(
        string transactionId,
        string expectedReceiptHash,
        string? famoDir = null)
    {
        string root = FullRoot(famoDir);
        using IDisposable held = UserDataTransactionLock.Acquire(root);
        Receipt receipt = ReadPinnedReceipt(root, transactionId,
            expectedReceiptHash, out string transactionRoot);
        string stagedRoot = Path.Combine(transactionRoot, "staged");
        for (int index = 0; index < receipt.Entries.Count; index++)
        {
            Entry entry = receipt.Entries[index];
            string live = SafePath(root, entry.RelativePath, allowMissingLeaf: true);
            string staged = SafePath(stagedRoot, entry.RelativePath,
                allowMissingLeaf: false);
            EnsureRegularFile(stagedRoot, staged);
            AtomicMutationResult applyResult = ApplyOwnedAtomic(
                staged, live, entry, transactionRoot, index);
            if (applyResult != AtomicMutationResult.Success)
            {
                return false;
            }
        }
        return true;
    }

    public static bool Rollback(
        string transactionId,
        string expectedReceiptHash,
        string? famoDir = null)
    {
        string root = FullRoot(famoDir);
        using IDisposable held = UserDataTransactionLock.Acquire(root);
        string transactionRoot = TransactionRoot(root, transactionId);
        if (TryReadRolledBackTombstone(
                root, transactionId, expectedReceiptHash))
        {
            if (Directory.Exists(transactionRoot))
            {
                DeleteTransactionTree(root, transactionRoot);
                AfterRollbackTransactionDeleteForTests?.Invoke();
            }
            return true;
        }
        if (!Directory.Exists(transactionRoot))
        {
            return false;
        }
        Receipt receipt = ReadPinnedReceipt(root, transactionId,
            expectedReceiptHash, out transactionRoot);
        string backupRoot = Path.Combine(transactionRoot, "backups");
        int preservedUserChanges = 0;
        for (int index = 0; index < receipt.Entries.Count; index++)
        {
            Entry entry = receipt.Entries[index];
            string live = SafePath(root, entry.RelativePath, allowMissingLeaf: true);
            AtomicMutationResult applyRecovery =
                ResolveApplyDisplacementForRollback(
                    live, entry, transactionRoot, index);
            if (applyRecovery == AtomicMutationResult.ConflictPreserved)
            {
                return false;
            }
            if (applyRecovery == AtomicMutationResult.ConflictRestored)
            {
                preservedUserChanges++;
                continue;
            }
            string owned = MutationPath(
                transactionRoot, index, "apply", "owned");
            bool ownedExists = MutationFileExists(
                transactionRoot, index, "apply", "owned");
            if (!entry.Existed &&
                MutationFileExists(transactionRoot, index,
                    "rollback-delete", "displaced"))
            {
                if (!ownedExists)
                {
                    return false;
                }
                AtomicMutationResult resumedDelete = DeleteVerifiedAtomic(
                    live, entry.AfterHash, transactionRoot, index);
                if (resumedDelete == AtomicMutationResult.ConflictPreserved)
                {
                    return false;
                }
                if (resumedDelete == AtomicMutationResult.ConflictRestored)
                {
                    preservedUserChanges++;
                }
                else
                {
                    DeleteEmptyParents(root, live);
                }
                DeleteMutationFile(transactionRoot, owned);
                continue;
            }
            if (entry.Existed &&
                MutationFileExists(transactionRoot, index,
                    "rollback", "displaced"))
            {
                if (!ownedExists)
                {
                    return false;
                }
                string resumedBackup = Path.Combine(
                    backupRoot, entry.BackupName);
                EnsureRegularFile(transactionRoot, resumedBackup);
                if (!string.Equals(HashFile(resumedBackup), entry.BackupHash,
                        StringComparison.Ordinal))
                {
                    throw new InvalidDataException(
                        "Seed transaction backup hash mismatch.");
                }
                string rollbackDisplaced = MutationPath(
                    transactionRoot, index, "rollback", "displaced");
                EnsureRegularFile(transactionRoot, rollbackDisplaced);
                EnsureRegularFile(transactionRoot, owned);
                bool exactOwnedDisplacement =
                    SameFileObject(rollbackDisplaced, owned) &&
                    string.Equals(
                        HashFile(rollbackDisplaced),
                        entry.AfterHash,
                        StringComparison.Ordinal);
                if (exactOwnedDisplacement && File.Exists(live))
                {
                    EnsureRegularFile(
                        Path.GetDirectoryName(live)
                        ?? throw new IOException(
                            "Seed live file has no parent."),
                        live);
                    if (!SameFileObject(live, rollbackDisplaced))
                    {
                        // Rollback already removed the exact transaction-owned
                        // object before the crash. A different object created
                        // at the live path afterwards is a user change. Retire
                        // only the proven transaction links and finish.
                        DeleteMutationFile(
                            transactionRoot, rollbackDisplaced);
                        DeleteMutationFile(
                            transactionRoot,
                            MutationPath(
                                transactionRoot,
                                index,
                                "rollback",
                                "incoming"));
                        DeleteMutationFile(transactionRoot, owned);
                        preservedUserChanges++;
                        continue;
                    }
                }
                AtomicMutationResult resumedRestore = CopyVerifiedAtomic(
                    resumedBackup, live, entry.BackupHash,
                    overwrite: true,
                    expectedDestinationHash: entry.AfterHash,
                    transactionRoot, index, "rollback");
                if (resumedRestore == AtomicMutationResult.ConflictPreserved)
                {
                    return false;
                }
                if (resumedRestore == AtomicMutationResult.ConflictRestored)
                {
                    preservedUserChanges++;
                }
                else if (!string.Equals(HashFile(live), entry.BeforeHash,
                             StringComparison.Ordinal))
                {
                    throw new IOException(
                        "Seed transaction backup restoration verification failed.");
                }
                DeleteMutationFile(transactionRoot, owned);
                continue;
            }
            bool liveExists = File.Exists(live);
            string currentHash = liveExists ? HashFile(live) : "";
            bool stillBefore = entry.Existed
                ? liveExists &&
                  string.Equals(currentHash, entry.BeforeHash,
                      StringComparison.Ordinal)
                : !liveExists;
            if (stillBefore)
            {
                DeleteMutationFile(transactionRoot, owned);
                continue;
            }
            if (!ownedExists)
            {
                // Matching bytes without the durable hard-link provenance are
                // owned by the other writer, not by this transaction.
                preservedUserChanges++;
                continue;
            }
            EnsureRegularFile(transactionRoot, owned);
            if (!liveExists || !SameFileObject(live, owned) ||
                !string.Equals(currentHash, entry.AfterHash,
                    StringComparison.Ordinal))
            {
                preservedUserChanges++;
                DeleteMutationFile(transactionRoot, owned);
                continue;
            }

            if (!entry.Existed)
            {
                AtomicMutationResult deleteResult =
                    DeleteVerifiedAtomic(
                        live, entry.AfterHash, transactionRoot, index);
                if (deleteResult == AtomicMutationResult.ConflictPreserved)
                {
                    return false;
                }
                if (deleteResult == AtomicMutationResult.ConflictRestored)
                {
                    preservedUserChanges++;
                    DeleteMutationFile(transactionRoot, owned);
                    continue;
                }
                DeleteEmptyParents(root, live);
                DeleteMutationFile(transactionRoot, owned);
                continue;
            }
            string backup = Path.Combine(backupRoot, entry.BackupName);
            EnsureRegularFile(transactionRoot, backup);
            if (!string.Equals(HashFile(backup), entry.BackupHash,
                    StringComparison.Ordinal))
            {
                throw new InvalidDataException(
                    "Seed transaction backup hash mismatch.");
            }
            AtomicMutationResult restoreResult = CopyVerifiedAtomic(
                backup, live, entry.BackupHash,
                overwrite: true, expectedDestinationHash: entry.AfterHash,
                transactionRoot, index, "rollback");
            if (restoreResult == AtomicMutationResult.ConflictPreserved)
            {
                return false;
            }
            if (restoreResult == AtomicMutationResult.ConflictRestored)
            {
                preservedUserChanges++;
                DeleteMutationFile(transactionRoot, owned);
                continue;
            }
            if (!string.Equals(HashFile(live), entry.BeforeHash,
                    StringComparison.Ordinal))
            {
                throw new IOException(
                    "Seed transaction backup restoration verification failed.");
            }
            DeleteMutationFile(transactionRoot, owned);
        }
        if (preservedUserChanges > 0)
        {
            FamoLog.Append(
                $"seed rollback preserved {preservedUserChanges} later user change(s); transaction={transactionId}");
        }
        WriteRolledBackTombstone(
            root, receipt.TransactionId, expectedReceiptHash);
        DeleteTransactionTree(root, transactionRoot);
        AfterRollbackTransactionDeleteForTests?.Invoke();
        return true;
    }

    public static bool Commit(
        string transactionId,
        string expectedReceiptHash,
        string? famoDir = null)
    {
        string root = FullRoot(famoDir);
        using IDisposable held = UserDataTransactionLock.Acquire(root);
        string transactionRoot = TransactionRoot(root, transactionId);
        if (!Directory.Exists(transactionRoot))
        {
            return true;
        }
        Receipt receipt = ReadPinnedReceipt(
            root, transactionId, expectedReceiptHash, out transactionRoot);
        if (HasUnresolvedMutationArtifacts(transactionRoot, receipt))
        {
            return false;
        }
        DeleteTransactionTree(root, transactionRoot);
        return true;
    }

    public static bool DiscardPrepared(string transactionId, string? famoDir = null)
    {
        string root = FullRoot(famoDir);
        using IDisposable held = UserDataTransactionLock.Acquire(root);
        ValidateTransactionId(transactionId);
        string transactionRoot = TransactionRoot(root, transactionId);
        if (!Directory.Exists(transactionRoot))
        {
            return true;
        }
        if (HasMutationArtifacts(transactionRoot))
        {
            return false;
        }
        DeleteTransactionTree(root, transactionRoot);
        return true;
    }

    private static Receipt ReadPinnedReceipt(
        string root,
        string transactionId,
        string expectedReceiptHash,
        out string transactionRoot)
    {
        ValidateTransactionId(transactionId);
        ValidateHash(expectedReceiptHash);
        EnsureDirectoryTree(root, create: false);
        transactionRoot = TransactionRoot(root, transactionId);
        EnsureDirectoryTree(transactionRoot, create: false);
        string receiptPath = Path.Combine(transactionRoot, ReceiptName);
        EnsureRegularFile(transactionRoot, receiptPath);
        byte[] receiptBytes = ReadBoundedFile(receiptPath, MaxReceiptBytes);
        string actualHash = Convert.ToHexString(SHA256.HashData(receiptBytes));
        if (!string.Equals(actualHash, expectedReceiptHash,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException("Seed receipt does not match the elevated journal hash.");
        }
        return ReadReceipt(receiptBytes, transactionId);
    }

    private static SortedSet<string> GetManagedFiles(string installedDataDir)
    {
        string sourceRoot = Path.GetFullPath(installedDataDir);
        EnsureDirectoryTree(sourceRoot, create: false);
        var managed = new SortedSet<string>(GeneratedFiles,
            StringComparer.OrdinalIgnoreCase);
        foreach (string path in EnumerateRegularFiles(sourceRoot))
        {
            managed.Add(NormalizeRelative(Path.GetRelativePath(sourceRoot, path)));
        }
        return managed;
    }

    private static IEnumerable<string> EnumerateRegularFiles(string root)
    {
        EnsureDirectoryTree(root, create: false);
        var pending = new Stack<string>();
        pending.Push(Path.GetFullPath(root));
        while (pending.Count > 0)
        {
            string directory = pending.Pop();
            foreach (string child in Directory.EnumerateFileSystemEntries(directory))
            {
                FileAttributes attributes = File.GetAttributes(child);
                if ((attributes & FileAttributes.ReparsePoint) != 0)
                {
                    throw new IOException($"Reparse points are not allowed: {child}");
                }
                if ((attributes & FileAttributes.Directory) != 0)
                {
                    pending.Push(child);
                }
                else
                {
                    yield return child;
                }
            }
        }
    }

    private static void EnsureDirectoryTree(string directory, bool create)
    {
        string full = Path.GetFullPath(directory);
        if (create && !Directory.Exists(full))
        {
            var missing = new Stack<string>();
            string? existing = full;
            while (existing is not null && !Directory.Exists(existing))
            {
                if (File.Exists(existing))
                {
                    throw new IOException(
                        $"Expected a directory but found a file: {existing}");
                }
                missing.Push(existing);
                string? parent = Path.GetDirectoryName(existing);
                if (string.Equals(parent, existing, StringComparison.Ordinal))
                {
                    break;
                }
                existing = parent;
            }
            if (existing is null || !Directory.Exists(existing))
            {
                throw new DirectoryNotFoundException(full);
            }
            // Validate the nearest existing ancestor before creating anything.
            // Directory.CreateDirectory(full) would otherwise traverse a
            // preexisting junction and write outside the Famo root before the
            // later safety check noticed.
            EnsureNoReparseChain(existing, includeLeaf: true);
            while (missing.Count > 0)
            {
                string next = missing.Pop();
                string parent = Path.GetDirectoryName(next)
                    ?? throw new IOException(
                        "Seed directory has no parent.");
                EnsureNoReparseChain(parent, includeLeaf: true);
                Directory.CreateDirectory(next);
                EnsureNoReparseChain(next, includeLeaf: true);
                if ((File.GetAttributes(next) &
                     FileAttributes.Directory) == 0)
                {
                    throw new IOException(
                        $"Expected a directory: {next}");
                }
            }
        }
        if (!Directory.Exists(full))
        {
            throw new DirectoryNotFoundException(full);
        }
        EnsureNoReparseChain(full, includeLeaf: true);
        if ((File.GetAttributes(full) & FileAttributes.Directory) == 0)
        {
            throw new IOException($"Expected a directory: {full}");
        }
    }

    private static void EnsureRegularFile(string root, string path)
    {
        string full = ContainedPath(root, path);
        EnsureNoReparseChain(full, includeLeaf: true);
        if (!File.Exists(full) ||
            (File.GetAttributes(full) &
             (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
        {
            throw new IOException($"Expected a regular file: {full}");
        }
    }

    private static void EnsureNoReparseChain(string path, bool includeLeaf)
    {
        string full = Path.GetFullPath(path);
        string? current = includeLeaf ? full : Path.GetDirectoryName(full);
        while (!string.IsNullOrEmpty(current))
        {
            if ((Directory.Exists(current) || File.Exists(current)) &&
                (File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
            {
                throw new IOException($"Reparse path component is not allowed: {current}");
            }
            string? parent = Path.GetDirectoryName(current);
            if (string.Equals(parent, current, StringComparison.Ordinal))
            {
                break;
            }
            current = parent;
        }
    }

    private static string SafePath(string root, string relative,
        bool allowMissingLeaf)
    {
        string full = ContainedPath(root,
            Path.Combine(root, NormalizeRelative(relative)
                .Replace('/', Path.DirectorySeparatorChar)));
        string? parent = Path.GetDirectoryName(full);
        if (parent is not null)
        {
            EnsureNoReparseChain(parent, includeLeaf: true);
        }
        if (!allowMissingLeaf || File.Exists(full) || Directory.Exists(full))
        {
            EnsureNoReparseChain(full, includeLeaf: true);
        }
        return full;
    }

    private static string ContainedPath(string root, string path)
    {
        string fullRoot = Path.GetFullPath(root).TrimEnd(
            Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        string fullPath = Path.GetFullPath(path);
        if (!string.Equals(fullPath, fullRoot, StringComparison.OrdinalIgnoreCase) &&
            !fullPath.StartsWith(fullRoot + Path.DirectorySeparatorChar,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new IOException("Seed path escaped its root.");
        }
        return fullPath;
    }

    private static string NormalizeRelative(string relative)
    {
        string normalized = relative.Replace('\\', '/');
        if (normalized.Length is 0 or > 512 || Path.IsPathRooted(relative) ||
            normalized.Split('/').Any(IsUnsafeSegment))
        {
            throw new IOException($"Unsafe seed relative path: {relative}");
        }
        return normalized;

        static bool IsUnsafeSegment(string segment)
        {
            if (segment is "" or "." or ".." || segment.Contains(':') ||
                segment.EndsWith(' ') || segment.EndsWith('.'))
            {
                return true;
            }
            string stem = segment.Split('.')[0];
            return stem.Equals("CON", StringComparison.OrdinalIgnoreCase) ||
                stem.Equals("PRN", StringComparison.OrdinalIgnoreCase) ||
                stem.Equals("AUX", StringComparison.OrdinalIgnoreCase) ||
                stem.Equals("NUL", StringComparison.OrdinalIgnoreCase) ||
                (stem.Length == 4 &&
                 (stem.StartsWith("COM", StringComparison.OrdinalIgnoreCase) ||
                  stem.StartsWith("LPT", StringComparison.OrdinalIgnoreCase)) &&
                 stem[3] is >= '1' and <= '9');
        }
    }

    private static string FullRoot(string? famoDir) =>
        Path.GetFullPath(famoDir ?? FamoPaths.FamoDir);

    private static string TransactionRoot(string root, string transactionId) =>
        Path.Combine(root, ".transactions", transactionId);

    private static string RolledBackTombstonePath(
        string root,
        string transactionId) =>
        Path.Combine(
            root,
            ".transactions",
            $"{transactionId}.rolledback.json");

    private static void ValidateTransactionId(string transactionId)
    {
        if (transactionId.Length != 32 ||
            transactionId.Any(c => !((c >= '0' && c <= '9') ||
                                     (c >= 'a' && c <= 'f'))))
        {
            throw new ArgumentException(
                "Transaction id must be 32 lowercase hex characters.",
                nameof(transactionId));
        }
    }

    private static void ValidateHash(string hash)
    {
        if (hash.Length != 64 ||
            hash.Any(c => !((c >= '0' && c <= '9') ||
                            (c >= 'A' && c <= 'F'))))
        {
            throw new ArgumentException(
                "Receipt hash must be 64 uppercase hex characters.", nameof(hash));
        }
    }

    private static string HashFile(string path)
    {
        using FileStream stream = new(path, FileMode.Open, FileAccess.Read,
            FileShare.Read);
        return Convert.ToHexString(SHA256.HashData(stream));
    }

    private static string HashReceiptFile(string path)
    {
        long size = new FileInfo(path).Length;
        if (size <= 0 || size > MaxReceiptBytes)
        {
            throw new InvalidDataException("Seed receipt size is invalid.");
        }
        return HashFile(path);
    }

    internal static void CopyDurableAtomic(
        string source, string destination, bool overwrite) =>
        CopyFileAtomic(source, destination, overwrite);

    internal static void WriteDurableAtomic(
        string destination, string content, bool overwrite = true)
    {
        ArgumentNullException.ThrowIfNull(content);
        WriteDurableAtomic(
            destination,
            Encoding.UTF8.GetBytes(content),
            overwrite);
    }

    internal static void WriteDurableAtomic(
        string destination, ReadOnlySpan<byte> content, bool overwrite = true)
    {
        destination = Path.GetFullPath(destination);
        using IDisposable held = UserDataTransactionLock.Acquire(destination);
        string parent = Path.GetDirectoryName(destination)
            ?? throw new IOException("Durable destination has no parent.");
        EnsureDirectoryTree(parent, create: false);
        EnsureNoReparseChain(destination, includeLeaf: false);
        string temporary =
            destination + $".famo-tmp-{Guid.NewGuid():N}";
        using DirectoryMutationLease lease =
            DirectoryMutationLease.Acquire(parent);
        using FileStream output = CreatePinnedTemporary(temporary);
        BeforeDurableArtifactWriteForTests?.Invoke(destination);
        output.Write(content);
        BeforeDurableArtifactFlushForTests?.Invoke(destination);
        output.Flush(flushToDisk: true);
        BeforeAtomicFilePublishForTests?.Invoke(destination);
        BeforePinnedAtomicFilePublishForTests?.Invoke(
            destination, temporary);
        using AtomicPublicationRecoveryLease recovery =
            CreateAtomicPublicationRecovery(
                output.SafeFileHandle,
                temporary,
                destination);
        lease.MovePinnedFile(
            output.SafeFileHandle,
            temporary,
            destination,
            overwrite);
        recovery.DeleteOnClose();
    }

    private static void CopyFileAtomic(
        string source, string destination, bool overwrite = true)
    {
        destination = Path.GetFullPath(destination);
        using IDisposable held = UserDataTransactionLock.Acquire(destination);
        EnsureNoReparseChain(source, includeLeaf: true);
        string parent = Path.GetDirectoryName(destination)
            ?? throw new IOException("Seed destination has no parent.");
        EnsureDirectoryTree(parent, create: true);
        EnsureNoReparseChain(destination, includeLeaf: false);
        string temporary = destination + $".famo-tmp-{Guid.NewGuid():N}";
        using DirectoryMutationLease lease = DirectoryMutationLease.Acquire(
            parent,
            Path.GetDirectoryName(source)
            ?? throw new IOException("Seed source has no parent."));
        using (var input = new FileStream(source, FileMode.Open,
                   FileAccess.Read, FileShare.Read))
        using (FileStream output = CreatePinnedTemporary(temporary))
        {
            BeforeDurableArtifactWriteForTests?.Invoke(destination);
            input.CopyTo(output);
            BeforeDurableArtifactFlushForTests?.Invoke(destination);
            output.Flush(flushToDisk: true);
            // A staged artifact can intentionally be rewritten onto itself.
            // Release only the read handle before replacing that destination;
            // the exact temporary object remains pinned through publication.
            input.Dispose();
            BeforeAtomicFilePublishForTests?.Invoke(destination);
            BeforePinnedAtomicFilePublishForTests?.Invoke(
                destination, temporary);
            using AtomicPublicationRecoveryLease recovery =
                CreateAtomicPublicationRecovery(
                    output.SafeFileHandle,
                    temporary,
                    destination);
            lease.MovePinnedFile(
                output.SafeFileHandle,
                temporary,
                destination,
                overwrite);
            recovery.DeleteOnClose();
        }
    }

    private static FileStream CreatePinnedTemporary(string path)
    {
        SafeFileHandle handle = NativeMethods.CreateFile(
            path,
            GenericWrite | DeleteAccess | FileReadAttributes,
            FileShare.None,
            IntPtr.Zero,
            CreateNew,
            FileAttributeTemporary | FileFlagSequentialScan |
            FileFlagWriteThrough | FileFlagOpenReparsePoint,
            IntPtr.Zero);
        if (handle.IsInvalid)
        {
            int error = Marshal.GetLastWin32Error();
            handle.Dispose();
            throw new IOException(
                $"Cannot create pinned seed temporary file: {path}",
                new Win32Exception(error));
        }
        try
        {
            SetDeleteDisposition(
                handle,
                delete: true,
                "Cannot arm kernel cleanup for a seed temporary file.");
            return new FileStream(
                handle,
                FileAccess.Write,
                bufferSize: 128 * 1024,
                isAsync: false);
        }
        catch
        {
            handle.Dispose();
            throw;
        }
    }

    private static void SetDeleteDisposition(
        SafeFileHandle handle, bool delete, string message)
    {
        var disposition = new FileDispositionInformation
        {
            DeleteFile = delete,
        };
        if (!NativeMethods.SetFileInformationByHandle(
                handle,
                FileDispositionInfo,
                ref disposition,
                (uint)Marshal.SizeOf<FileDispositionInformation>()))
        {
            throw new IOException(
                message,
                new Win32Exception(Marshal.GetLastWin32Error()));
        }
    }

    internal static void RecoverPendingAtomicPublication(string dataRoot)
    {
        string recoveryPath =
            UserDataTransactionLock.AtomicPublicationRecoveryPath(dataRoot);
        string recoveryParent = Path.GetDirectoryName(recoveryPath)
            ?? throw new IOException(
                "Atomic publication recovery path has no parent.");
        using DirectoryMutationLease recoveryDirectory =
            DirectoryMutationLease.Acquire(recoveryParent);
        SafeFileHandle handle = NativeMethods.CreateFile(
            recoveryPath,
            GenericRead | DeleteAccess | FileReadAttributes,
            FileShare.None,
            IntPtr.Zero,
            OpenExisting,
            FileFlagSequentialScan | FileFlagOpenReparsePoint,
            IntPtr.Zero);
        if (handle.IsInvalid)
        {
            int error = Marshal.GetLastWin32Error();
            handle.Dispose();
            if (error is ErrorFileNotFound or ErrorPathNotFound)
            {
                return;
            }
            throw new IOException(
                "Cannot open the atomic publication recovery record.",
                new Win32Exception(error));
        }

        try
        {
            ValidatePinnedRegularFile(
                handle,
                recoveryPath,
                "Unsafe atomic publication recovery record.");
            using var input = new FileStream(
                handle,
                FileAccess.Read,
                bufferSize: 4096,
                isAsync: false);
            AtomicPublicationRecoveryRecord? record =
                ReadAtomicPublicationRecovery(input);
            if (record is not null)
            {
                RecoverExactAtomicPublicationTemporary(record);
            }
            SetDeleteDisposition(
                input.SafeFileHandle,
                delete: true,
                "Cannot remove the atomic publication recovery record.");
        }
        catch
        {
            handle.Dispose();
            throw;
        }
    }

    private static AtomicPublicationRecoveryLease
        CreateAtomicPublicationRecovery(
            SafeFileHandle temporaryHandle,
            string temporary,
            string destination)
    {
        FileObjectIdentity identity =
            GetFileObjectIdentity(temporaryHandle);
        var record = new AtomicPublicationRecoveryRecord
        {
            Schema = AtomicPublicationRecoverySchema,
            Temporary = Path.GetFullPath(temporary),
            Destination = Path.GetFullPath(destination),
            VolumeSerialNumber = identity.VolumeSerialNumber,
            FileIndex = identity.FileIndex,
        };
        ValidateAtomicPublicationRecoveryRecord(record);
        byte[] envelope = SerializeAtomicPublicationRecovery(record);
        string recoveryPath =
            UserDataTransactionLock.AtomicPublicationRecoveryPath(destination);
        string recoveryParent = Path.GetDirectoryName(recoveryPath)
            ?? throw new IOException(
                "Atomic publication recovery path has no parent.");
        using DirectoryMutationLease recoveryDirectory =
            DirectoryMutationLease.Acquire(recoveryParent);
        SafeFileHandle handle = NativeMethods.CreateFile(
            recoveryPath,
            GenericRead | GenericWrite |
            DeleteAccess | FileReadAttributes,
            FileShare.None,
            IntPtr.Zero,
            CreateNew,
            FileAttributeNormal | FileFlagWriteThrough |
            FileFlagOpenReparsePoint,
            IntPtr.Zero);
        if (handle.IsInvalid)
        {
            int error = Marshal.GetLastWin32Error();
            handle.Dispose();
            throw new IOException(
                "Cannot create the atomic publication recovery record.",
                new Win32Exception(error));
        }
        try
        {
            ValidatePinnedRegularFile(
                handle,
                recoveryPath,
                "Unsafe atomic publication recovery record.");
            var output = new FileStream(
                handle,
                FileAccess.ReadWrite,
                bufferSize: 4096,
                isAsync: false);
            try
            {
                output.Write(envelope);
                output.Flush(flushToDisk: true);
                return new AtomicPublicationRecoveryLease(output);
            }
            catch
            {
                output.Dispose();
                throw;
            }
        }
        catch
        {
            handle.Dispose();
            throw;
        }
    }

    private static byte[] SerializeAtomicPublicationRecovery(
        AtomicPublicationRecoveryRecord record)
    {
        byte[] payload = JsonSerializer.SerializeToUtf8Bytes(record);
        int envelopeLength = checked(
            AtomicPublicationRecoveryHeader.Length +
            sizeof(int) +
            payload.Length +
            SHA256.HashSizeInBytes +
            AtomicPublicationRecoveryCommit.Length);
        if (envelopeLength > MaxAtomicPublicationRecoveryBytes)
        {
            throw new IOException(
                "Atomic publication recovery record is too large.");
        }
        byte[] envelope = new byte[envelopeLength];
        int offset = 0;
        AtomicPublicationRecoveryHeader.CopyTo(envelope, offset);
        offset += AtomicPublicationRecoveryHeader.Length;
        BinaryPrimitives.WriteInt32LittleEndian(
            envelope.AsSpan(offset, sizeof(int)),
            payload.Length);
        offset += sizeof(int);
        payload.CopyTo(envelope, offset);
        offset += payload.Length;
        SHA256.HashData(
            payload,
            envelope.AsSpan(offset, SHA256.HashSizeInBytes));
        offset += SHA256.HashSizeInBytes;
        AtomicPublicationRecoveryCommit.CopyTo(envelope, offset);
        return envelope;
    }

    private static AtomicPublicationRecoveryRecord?
        ReadAtomicPublicationRecovery(FileStream input)
    {
        if (input.Length > MaxAtomicPublicationRecoveryBytes)
        {
            throw new InvalidDataException(
                "Atomic publication recovery record is too large.");
        }
        byte[] envelope = new byte[checked((int)input.Length)];
        input.ReadExactly(envelope);
        if (envelope.Length < AtomicPublicationRecoveryCommit.Length ||
            !envelope.AsSpan(
                    envelope.Length - AtomicPublicationRecoveryCommit.Length)
                .SequenceEqual(AtomicPublicationRecoveryCommit))
        {
            // Publication never clears delete disposition until this exact
            // commit marker has reached stable storage. A torn, uncommitted
            // record therefore cannot own a persistent temporary object.
            return null;
        }
        int minimumLength =
            AtomicPublicationRecoveryHeader.Length +
            sizeof(int) +
            SHA256.HashSizeInBytes +
            AtomicPublicationRecoveryCommit.Length;
        if (envelope.Length < minimumLength ||
            !envelope.AsSpan(0, AtomicPublicationRecoveryHeader.Length)
                .SequenceEqual(AtomicPublicationRecoveryHeader))
        {
            throw new InvalidDataException(
                "Atomic publication recovery record header is invalid.");
        }
        int payloadOffset =
            AtomicPublicationRecoveryHeader.Length + sizeof(int);
        int payloadLength = BinaryPrimitives.ReadInt32LittleEndian(
            envelope.AsSpan(
                AtomicPublicationRecoveryHeader.Length,
                sizeof(int)));
        int expectedLength;
        try
        {
            expectedLength = checked(
                payloadOffset +
                payloadLength +
                SHA256.HashSizeInBytes +
                AtomicPublicationRecoveryCommit.Length);
        }
        catch (OverflowException ex)
        {
            throw new InvalidDataException(
                "Atomic publication recovery record length is invalid.",
                ex);
        }
        if (payloadLength <= 0 || expectedLength != envelope.Length)
        {
            throw new InvalidDataException(
                "Atomic publication recovery record length is invalid.");
        }
        ReadOnlySpan<byte> payload =
            envelope.AsSpan(payloadOffset, payloadLength);
        ReadOnlySpan<byte> recordedDigest = envelope.AsSpan(
            payloadOffset + payloadLength,
            SHA256.HashSizeInBytes);
        Span<byte> actualDigest = stackalloc byte[SHA256.HashSizeInBytes];
        SHA256.HashData(payload, actualDigest);
        if (!CryptographicOperations.FixedTimeEquals(
                actualDigest,
                recordedDigest))
        {
            throw new InvalidDataException(
                "Atomic publication recovery record digest is invalid.");
        }
        AtomicPublicationRecoveryRecord record;
        try
        {
            record =
                JsonSerializer.Deserialize<AtomicPublicationRecoveryRecord>(
                    payload)
                ?? throw new InvalidDataException(
                    "Atomic publication recovery record is empty.");
        }
        catch (JsonException ex)
        {
            throw new InvalidDataException(
                "Atomic publication recovery record JSON is invalid.",
                ex);
        }
        ValidateAtomicPublicationRecoveryRecord(record);
        return record;
    }

    private static void ValidateAtomicPublicationRecoveryRecord(
        AtomicPublicationRecoveryRecord record)
    {
        if (!string.Equals(
                record.Schema,
                AtomicPublicationRecoverySchema,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "Atomic publication recovery record schema is invalid.");
        }
        string temporary;
        string destination;
        try
        {
            temporary = Path.GetFullPath(record.Temporary);
            destination = Path.GetFullPath(record.Destination);
        }
        catch (Exception ex) when (
            ex is ArgumentException or NotSupportedException or
            PathTooLongException)
        {
            throw new InvalidDataException(
                "Atomic publication recovery path is invalid.",
                ex);
        }
        if (!string.Equals(
                temporary,
                record.Temporary,
                StringComparison.OrdinalIgnoreCase) ||
            !string.Equals(
                destination,
                record.Destination,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                "Atomic publication recovery path is not canonical.");
        }
        string prefix = destination + ".famo-tmp-";
        if (!temporary.StartsWith(
                prefix,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                "Atomic publication recovery temporary path is invalid.");
        }
        string suffix = temporary[prefix.Length..];
        if (suffix.Length != 32 || suffix.Any(character =>
                !Uri.IsHexDigit(character)))
        {
            throw new InvalidDataException(
                "Atomic publication recovery temporary name is invalid.");
        }
        if (record.FileIndex == 0)
        {
            throw new InvalidDataException(
                "Atomic publication recovery object identity is invalid.");
        }
    }

    private static void RecoverExactAtomicPublicationTemporary(
        AtomicPublicationRecoveryRecord record)
    {
        string parent = Path.GetDirectoryName(record.Temporary)
            ?? throw new InvalidDataException(
                "Atomic publication temporary path has no parent.");
        if (!Directory.Exists(parent))
        {
            FlushPublishedAtomicPublicationIfPresent(record);
            return;
        }
        using DirectoryMutationLease temporaryDirectory =
            DirectoryMutationLease.Acquire(parent);
        SafeFileHandle handle = NativeMethods.CreateFile(
            record.Temporary,
            GenericWrite | DeleteAccess | FileReadAttributes,
            FileShare.Read | FileShare.Write,
            IntPtr.Zero,
            OpenExisting,
            FileFlagWriteThrough | FileFlagOpenReparsePoint,
            IntPtr.Zero);
        if (handle.IsInvalid)
        {
            int error = Marshal.GetLastWin32Error();
            handle.Dispose();
            if (error is ErrorFileNotFound or ErrorPathNotFound)
            {
                FlushPublishedAtomicPublicationIfPresent(record);
                return;
            }
            throw new IOException(
                "Cannot open the pending atomic publication temporary.",
                new Win32Exception(error));
        }
        using (handle)
        {
            ValidatePinnedRegularFile(
                handle,
                record.Temporary,
                "Unsafe pending atomic publication temporary.");
            FileObjectIdentity actual = GetFileObjectIdentity(handle);
            var expected = new FileObjectIdentity(
                record.VolumeSerialNumber,
                record.FileIndex);
            if (actual != expected)
            {
                throw new IOException(
                    "Pending atomic publication temporary identity changed.");
            }
            SetDeleteDisposition(
                handle,
                delete: true,
                "Cannot remove the pending atomic publication temporary.");
            FlushPinnedFile(
                handle,
                "Cannot flush pending atomic publication cleanup.");
        }
    }

    private static void FlushPublishedAtomicPublicationIfPresent(
        AtomicPublicationRecoveryRecord record)
    {
        string parent = Path.GetDirectoryName(record.Destination)
            ?? throw new InvalidDataException(
                "Atomic publication destination path has no parent.");
        if (!Directory.Exists(parent))
        {
            return;
        }
        using DirectoryMutationLease destinationDirectory =
            DirectoryMutationLease.Acquire(parent);
        SafeFileHandle handle = NativeMethods.CreateFile(
            record.Destination,
            GenericWrite | FileReadAttributes,
            FileShare.Read | FileShare.Write,
            IntPtr.Zero,
            OpenExisting,
            FileFlagWriteThrough | FileFlagOpenReparsePoint,
            IntPtr.Zero);
        if (handle.IsInvalid)
        {
            int error = Marshal.GetLastWin32Error();
            handle.Dispose();
            if (error is ErrorFileNotFound or ErrorPathNotFound)
            {
                return;
            }
            throw new IOException(
                "Cannot open the published atomic destination for recovery.",
                new Win32Exception(error));
        }
        using (handle)
        {
            ValidatePinnedRegularFile(
                handle,
                record.Destination,
                "Unsafe published atomic destination.");
            FileObjectIdentity actual = GetFileObjectIdentity(handle);
            var expected = new FileObjectIdentity(
                record.VolumeSerialNumber,
                record.FileIndex);
            if (actual == expected)
            {
                FlushPinnedFile(
                    handle,
                    "Cannot flush the recovered atomic destination.");
            }
        }
    }

    private static void FlushPinnedFile(
        SafeFileHandle handle,
        string message)
    {
        if (!NativeMethods.FlushFileBuffers(handle))
        {
            throw new IOException(
                message,
                new Win32Exception(Marshal.GetLastWin32Error()));
        }
    }

    private static void ValidatePinnedRegularFile(
        SafeFileHandle handle,
        string expectedPath,
        string message)
    {
        if (!NativeMethods.GetFileInformationByHandle(
                handle,
                out ByHandleFileInformation information) ||
            (information.FileAttributes &
             (FileAttributeDirectory | FileAttributeReparsePoint)) != 0)
        {
            throw new IOException(message);
        }
        ulong index = ((ulong)information.FileIndexHigh << 32) |
            information.FileIndexLow;
        if (index == 0)
        {
            throw new IOException(message);
        }
        var finalPath = new StringBuilder(32_768);
        uint length = NativeMethods.GetFinalPathNameByHandle(
            handle,
            finalPath,
            (uint)finalPath.Capacity,
            0);
        if (length == 0 || length >= finalPath.Capacity ||
            !string.Equals(
                NormalizeFinalFilePath(finalPath.ToString()),
                Path.GetFullPath(expectedPath),
                StringComparison.OrdinalIgnoreCase))
        {
            throw new IOException(message);
        }
    }

    private static string NormalizeFinalFilePath(string path)
    {
        if (path.StartsWith(
                @"\\?\UNC\",
                StringComparison.OrdinalIgnoreCase))
        {
            path = @"\\" + path[8..];
        }
        else if (path.StartsWith(
                     @"\\?\",
                     StringComparison.OrdinalIgnoreCase))
        {
            path = path[4..];
        }
        return Path.GetFullPath(path);
    }

    private static AtomicMutationResult ApplyOwnedAtomic(
        string source, string destination, Entry entry,
        string transactionRoot, int index)
    {
        string parent = Path.GetDirectoryName(destination)
            ?? throw new IOException("Seed destination has no parent.");
        EnsureDirectoryTree(parent, create: true);
        EnsureNoReparseChain(destination, includeLeaf: false);
        string incoming = MutationPath(
            transactionRoot, index, "apply", "incoming");
        string displaced = MutationPath(
            transactionRoot, index, "apply", "displaced");
        string owned = MutationPath(
            transactionRoot, index, "apply", "owned");
        using DirectoryMutationLease lease = DirectoryMutationLease.Acquire(
            parent,
            Path.GetDirectoryName(source)
            ?? throw new IOException("Seed source has no parent."),
            Path.GetDirectoryName(incoming)
            ?? throw new IOException("Seed quarantine path has no parent."));

        if (File.Exists(owned))
        {
            EnsureRegularFile(transactionRoot, owned);
            if (!string.Equals(HashFile(owned), entry.AfterHash,
                    StringComparison.Ordinal))
            {
                return AtomicMutationResult.ConflictPreserved;
            }
            if (File.Exists(destination))
            {
                EnsureRegularFile(parent, destination);
                if (SameFileObject(destination, owned))
                {
                    if (!string.Equals(HashFile(destination), entry.AfterHash,
                            StringComparison.Ordinal))
                    {
                        return AtomicMutationResult.ConflictPreserved;
                    }
                    if (File.Exists(displaced))
                    {
                        EnsureRegularFile(transactionRoot, displaced);
                        if (!entry.Existed ||
                            !string.Equals(HashFile(displaced), entry.BeforeHash,
                                StringComparison.Ordinal))
                        {
                            return AtomicMutationResult.ConflictPreserved;
                        }
                        File.Delete(displaced);
                    }
                    DeleteMutationFile(transactionRoot, incoming);
                    return AtomicMutationResult.Success;
                }
                // An object other than the one installed by this transaction
                // wins, even when its bytes are identical.
                return AtomicMutationResult.ConflictPreserved;
            }
            if (Directory.Exists(destination))
            {
                return AtomicMutationResult.ConflictPreserved;
            }
            if (!File.Exists(incoming))
            {
                // The owned source was moved at least once, but the live link
                // is now absent. Treat that as a later deletion, not as
                // permission to recreate the file from matching bytes.
                return AtomicMutationResult.ConflictPreserved;
            }
            EnsureRegularFile(transactionRoot, incoming);
            if (!SameFileObject(incoming, owned))
            {
                return AtomicMutationResult.ConflictPreserved;
            }
        }
        else
        {
            if (File.Exists(displaced))
            {
                EnsureRegularFile(transactionRoot, displaced);
                // A valid before-image may be restored when provenance was
                // lost before installation. A mismatched crash artifact is
                // never copied back to live.
                if (entry.Existed &&
                    string.Equals(HashFile(displaced), entry.BeforeHash,
                        StringComparison.Ordinal) &&
                    !File.Exists(destination) &&
                    !Directory.Exists(destination) &&
                    RestoreDisplaced(destination, displaced, lease))
                {
                    DeleteMutationFile(transactionRoot, incoming);
                    return AtomicMutationResult.ConflictRestored;
                }
                return AtomicMutationResult.ConflictPreserved;
            }

            string currentHash = File.Exists(destination)
                ? HashFile(destination)
                : "";
            bool stillBefore = entry.Existed
                ? File.Exists(destination) &&
                  string.Equals(currentHash, entry.BeforeHash,
                      StringComparison.Ordinal)
                : !File.Exists(destination) && !Directory.Exists(destination);
            if (!stillBefore)
            {
                // Hash equality with AfterHash is intentionally insufficient:
                // another writer can create the same bytes after Prepare.
                return AtomicMutationResult.ConflictRestored;
            }

            PrepareIncoming(source, incoming, entry.AfterHash);
            CreateOwnershipAnchor(incoming, owned);
        }

        EnsureRegularFile(transactionRoot, incoming);
        EnsureRegularFile(transactionRoot, owned);
        if (!SameFileObject(incoming, owned) ||
            !string.Equals(HashFile(incoming), entry.AfterHash,
                StringComparison.Ordinal))
        {
            return AtomicMutationResult.ConflictPreserved;
        }

        if (entry.Existed)
        {
            if (!File.Exists(displaced))
            {
                if (!File.Exists(destination))
                {
                    return AtomicMutationResult.ConflictRestored;
                }
                EnsureRegularFile(parent, destination);
                if (!string.Equals(HashFile(destination), entry.BeforeHash,
                        StringComparison.Ordinal))
                {
                    DeletePreparedOwnership(
                        transactionRoot, incoming, owned);
                    return AtomicMutationResult.ConflictRestored;
                }
                try
                {
                    using DirectoryMutationLease.PreparedFileMove move =
                        lease.PrepareMove(
                            destination, displaced, overwrite: false);
                    BeforeAtomicMutationForTests?.Invoke(destination);
                    move.Execute();
                }
                catch (IOException)
                {
                    DeletePreparedOwnership(
                        transactionRoot, incoming, owned);
                    return AtomicMutationResult.ConflictRestored;
                }
                AfterAtomicDisplacementForTests?.Invoke(destination);
                EnsureRegularFile(transactionRoot, displaced);
                if (!string.Equals(HashFile(displaced), entry.BeforeHash,
                        StringComparison.Ordinal))
                {
                    bool restored = RestoreDisplaced(
                        destination, displaced, lease);
                    if (restored)
                    {
                        DeletePreparedOwnership(
                            transactionRoot, incoming, owned);
                    }
                    return restored
                        ? AtomicMutationResult.ConflictRestored
                        : AtomicMutationResult.ConflictPreserved;
                }
            }
            else
            {
                EnsureRegularFile(transactionRoot, displaced);
                if (!string.Equals(HashFile(displaced), entry.BeforeHash,
                        StringComparison.Ordinal))
                {
                    // This is a resumed artifact, not one displaced in this
                    // invocation, so it has no authority to repopulate live.
                    return AtomicMutationResult.ConflictPreserved;
                }
                if (File.Exists(destination) || Directory.Exists(destination))
                {
                    return AtomicMutationResult.ConflictPreserved;
                }
            }
        }
        else
        {
            if (File.Exists(displaced) ||
                File.Exists(destination) || Directory.Exists(destination))
            {
                DeletePreparedOwnership(transactionRoot, incoming, owned);
                return AtomicMutationResult.ConflictRestored;
            }
            BeforeAtomicMutationForTests?.Invoke(destination);
        }

        try
        {
            lease.MoveFile(incoming, destination, overwrite: false);
        }
        catch (IOException)
        {
            if (entry.Existed &&
                !File.Exists(destination) &&
                !Directory.Exists(destination) &&
                File.Exists(displaced) &&
                string.Equals(HashFile(displaced), entry.BeforeHash,
                    StringComparison.Ordinal) &&
                RestoreDisplaced(destination, displaced, lease))
            {
                DeletePreparedOwnership(transactionRoot, incoming, owned);
                return AtomicMutationResult.ConflictRestored;
            }
            if (!entry.Existed)
            {
                DeletePreparedOwnership(transactionRoot, incoming, owned);
            }
            return AtomicMutationResult.ConflictPreserved;
        }
        AfterAtomicInstallForTests?.Invoke(destination);
        EnsureRegularFile(parent, destination);
        EnsureRegularFile(transactionRoot, owned);
        if (!SameFileObject(destination, owned) ||
            !string.Equals(HashFile(destination), entry.AfterHash,
                StringComparison.Ordinal))
        {
            return AtomicMutationResult.ConflictPreserved;
        }
        if (entry.Existed)
        {
            EnsureRegularFile(transactionRoot, displaced);
            if (!string.Equals(HashFile(displaced), entry.BeforeHash,
                    StringComparison.Ordinal))
            {
                return AtomicMutationResult.ConflictPreserved;
            }
            File.Delete(displaced);
        }
        return AtomicMutationResult.Success;
    }

    private static AtomicMutationResult CopyVerifiedAtomic(
        string source, string destination, string expectedHash, bool overwrite,
        string expectedDestinationHash, string transactionRoot, int index,
        string operation)
    {
        ValidateHash(expectedHash);
        if (overwrite)
        {
            ValidateHash(expectedDestinationHash);
        }
        EnsureNoReparseChain(source, includeLeaf: true);
        string parent = Path.GetDirectoryName(destination)
            ?? throw new IOException("Seed destination has no parent.");
        EnsureDirectoryTree(parent, create: true);
        EnsureNoReparseChain(destination, includeLeaf: false);
        string incoming = MutationPath(
            transactionRoot, index, operation, "incoming");
        string displaced = MutationPath(
            transactionRoot, index, operation, "displaced");
        using DirectoryMutationLease lease = DirectoryMutationLease.Acquire(
            parent,
            Path.GetDirectoryName(source)
            ?? throw new IOException("Seed source has no parent."),
            Path.GetDirectoryName(incoming)
            ?? throw new IOException("Seed quarantine path has no parent."));
        PrepareIncoming(source, incoming, expectedHash);

        if (File.Exists(displaced))
        {
            EnsureRegularFile(transactionRoot, displaced);
            if (!string.Equals(HashFile(displaced),
                    expectedDestinationHash, StringComparison.Ordinal))
            {
                // A resumed deterministic artifact may have been replaced
                // after the crash. Never give those bytes authority over live.
                if (File.Exists(destination))
                {
                    EnsureRegularFile(parent, destination);
                    if (string.Equals(HashFile(destination), expectedHash,
                            StringComparison.Ordinal))
                    {
                        File.Delete(displaced);
                        DeleteMutationFile(transactionRoot, incoming);
                        return AtomicMutationResult.Success;
                    }
                    return AtomicMutationResult.ConflictPreserved;
                }
                if (!Directory.Exists(destination) &&
                    string.Equals(operation, "rollback",
                        StringComparison.Ordinal))
                {
                    try
                    {
                        // The independently verified backup, not the bad
                        // displaced object, restores the missing live file.
                        lease.MoveFile(
                            incoming, destination, overwrite: false);
                    }
                    catch (IOException)
                    {
                        return AtomicMutationResult.ConflictPreserved;
                    }
                    AfterAtomicInstallForTests?.Invoke(destination);
                    EnsureRegularFile(parent, destination);
                    if (!string.Equals(HashFile(destination), expectedHash,
                            StringComparison.Ordinal))
                    {
                        return AtomicMutationResult.ConflictPreserved;
                    }
                    File.Delete(displaced);
                    return AtomicMutationResult.Success;
                }
                return AtomicMutationResult.ConflictPreserved;
            }
            if (File.Exists(destination))
            {
                EnsureRegularFile(parent, destination);
                if (string.Equals(HashFile(destination), expectedHash,
                        StringComparison.Ordinal))
                {
                    File.Delete(displaced);
                    DeleteMutationFile(transactionRoot, incoming);
                    return AtomicMutationResult.Success;
                }
                return AtomicMutationResult.ConflictPreserved;
            }
            if (Directory.Exists(destination))
            {
                return AtomicMutationResult.ConflictPreserved;
            }
            try
            {
                lease.MoveFile(incoming, destination, overwrite: false);
            }
            catch (IOException)
            {
                return AtomicMutationResult.ConflictPreserved;
            }
            AfterAtomicInstallForTests?.Invoke(destination);
            EnsureRegularFile(parent, destination);
            if (!string.Equals(HashFile(destination), expectedHash,
                    StringComparison.Ordinal))
            {
                return AtomicMutationResult.ConflictPreserved;
            }
            File.Delete(displaced);
            return AtomicMutationResult.Success;
        }

        if (File.Exists(destination))
        {
            EnsureRegularFile(parent, destination);
            string liveHash = HashFile(destination);
            if (string.Equals(liveHash, expectedHash, StringComparison.Ordinal))
            {
                DeleteMutationFile(transactionRoot, incoming);
                return AtomicMutationResult.Success;
            }
            if (!overwrite ||
                !string.Equals(liveHash, expectedDestinationHash,
                    StringComparison.Ordinal))
            {
                return AtomicMutationResult.ConflictRestored;
            }
        }
        else if (Directory.Exists(destination) || overwrite)
        {
            return AtomicMutationResult.ConflictRestored;
        }

        if (overwrite)
        {
            try
            {
                // The deterministic transaction quarantine makes a crash
                // after this rename discoverable and idempotently recoverable.
                using DirectoryMutationLease.PreparedFileMove move =
                    lease.PrepareMove(
                        destination, displaced, overwrite: false);
                BeforeAtomicMutationForTests?.Invoke(destination);
                move.Execute();
            }
            catch (IOException)
            {
                return AtomicMutationResult.ConflictRestored;
            }
            AfterAtomicDisplacementForTests?.Invoke(destination);
            EnsureRegularFile(transactionRoot, displaced);
            if (!string.Equals(HashFile(displaced),
                    expectedDestinationHash, StringComparison.Ordinal))
            {
                return RestoreDisplaced(destination, displaced, lease)
                    ? AtomicMutationResult.ConflictRestored
                    : AtomicMutationResult.ConflictPreserved;
            }
        }
        else
        {
            BeforeAtomicMutationForTests?.Invoke(destination);
        }
        try
        {
            lease.MoveFile(incoming, destination, overwrite: false);
        }
        catch (IOException)
        {
            if (!overwrite)
            {
                return AtomicMutationResult.ConflictRestored;
            }
            return RestoreDisplaced(destination, displaced, lease)
                ? AtomicMutationResult.ConflictRestored
                : AtomicMutationResult.ConflictPreserved;
        }
        AfterAtomicInstallForTests?.Invoke(destination);
        EnsureRegularFile(parent, destination);
        if (!string.Equals(HashFile(destination), expectedHash,
                StringComparison.Ordinal))
        {
            return AtomicMutationResult.ConflictPreserved;
        }
        if (overwrite)
        {
            File.Delete(displaced);
        }
        return AtomicMutationResult.Success;
    }

    private static AtomicMutationResult DeleteVerifiedAtomic(
        string destination, string expectedHash, string transactionRoot,
        int index)
    {
        ValidateHash(expectedHash);
        EnsureNoReparseChain(destination, includeLeaf: false);
        string displaced = MutationPath(
            transactionRoot, index, "rollback-delete", "displaced");
        using DirectoryMutationLease lease = DirectoryMutationLease.Acquire(
            Path.GetDirectoryName(destination)
            ?? throw new IOException("Seed destination has no parent."),
            Path.GetDirectoryName(displaced)
            ?? throw new IOException("Seed quarantine path has no parent."));
        if (File.Exists(displaced))
        {
            EnsureRegularFile(transactionRoot, displaced);
            if (!string.Equals(HashFile(displaced), expectedHash,
                    StringComparison.Ordinal))
            {
                // The desired rollback state for a newly created file is
                // absence. A bad resumed quarantine object is deleted or kept
                // as debt; it is never moved into live.
                if (!File.Exists(destination) &&
                    !Directory.Exists(destination))
                {
                    File.Delete(displaced);
                    return AtomicMutationResult.Success;
                }
                File.Delete(displaced);
                return AtomicMutationResult.ConflictRestored;
            }
            if (File.Exists(destination) || Directory.Exists(destination))
            {
                File.Delete(displaced);
                return AtomicMutationResult.ConflictRestored;
            }
            File.Delete(displaced);
            return AtomicMutationResult.Success;
        }
        if (!File.Exists(destination))
        {
            return Directory.Exists(destination)
                ? AtomicMutationResult.ConflictRestored
                : AtomicMutationResult.Success;
        }
        EnsureRegularFile(
            Path.GetDirectoryName(destination)
            ?? throw new IOException("Seed destination has no parent."),
            destination);
        if (!string.Equals(HashFile(destination), expectedHash,
                StringComparison.Ordinal))
        {
            return AtomicMutationResult.ConflictRestored;
        }
        try
        {
            using DirectoryMutationLease.PreparedFileMove move =
                lease.PrepareMove(
                    destination, displaced, overwrite: false);
            BeforeAtomicMutationForTests?.Invoke(destination);
            move.Execute();
        }
        catch (IOException)
        {
            return AtomicMutationResult.ConflictRestored;
        }
        AfterAtomicDisplacementForTests?.Invoke(destination);
        EnsureRegularFile(transactionRoot, displaced);
        if (!string.Equals(HashFile(displaced), expectedHash,
                StringComparison.Ordinal))
        {
            return RestoreDisplaced(destination, displaced, lease)
                ? AtomicMutationResult.ConflictRestored
                : AtomicMutationResult.ConflictPreserved;
        }
        File.Delete(displaced);
        return AtomicMutationResult.Success;
    }

    private static AtomicMutationResult ResolveApplyDisplacementForRollback(
        string destination, Entry entry, string transactionRoot, int index)
    {
        string incoming = MutationPath(
            transactionRoot, index, "apply", "incoming");
        string displaced = MutationPath(
            transactionRoot, index, "apply", "displaced");
        string owned = MutationPath(
            transactionRoot, index, "apply", "owned");
        using DirectoryMutationLease lease = DirectoryMutationLease.Acquire(
            Path.GetDirectoryName(destination)
            ?? throw new IOException("Seed destination has no parent."),
            Path.GetDirectoryName(incoming)
            ?? throw new IOException("Seed quarantine path has no parent."));
        if (!File.Exists(displaced))
        {
            if (File.Exists(incoming))
            {
                // Ownership was prepared, but the live CAS displacement never
                // happened. Removing both hard links leaves live untouched.
                DeletePreparedOwnership(transactionRoot, incoming, owned);
            }
            return AtomicMutationResult.Success;
        }
        if (!entry.Existed)
        {
            return AtomicMutationResult.ConflictPreserved;
        }
        EnsureRegularFile(transactionRoot, displaced);
        if (!string.Equals(HashFile(displaced), entry.BeforeHash,
                StringComparison.Ordinal))
        {
            // This artifact survived a prior invocation and failed its pinned
            // hash, so it must never be restored. If installation had not yet
            // consumed incoming, only the independently verified Prepare
            // backup may reconstruct the missing before-image.
            if (!File.Exists(destination) &&
                !Directory.Exists(destination) &&
                File.Exists(incoming))
            {
                string backup = Path.Combine(
                    transactionRoot, "backups", entry.BackupName);
                EnsureRegularFile(transactionRoot, backup);
                if (!string.Equals(HashFile(backup), entry.BackupHash,
                        StringComparison.Ordinal))
                {
                    return AtomicMutationResult.ConflictPreserved;
                }
                string recoveryIncoming = MutationPath(
                    transactionRoot, index, "apply-recovery", "incoming");
                PrepareIncoming(
                    backup, recoveryIncoming, entry.BeforeHash);
                try
                {
                    lease.MoveFile(
                        recoveryIncoming, destination, overwrite: false);
                }
                catch (IOException)
                {
                    return AtomicMutationResult.ConflictPreserved;
                }
                EnsureRegularFile(
                    Path.GetDirectoryName(destination)
                    ?? throw new IOException(
                        "Seed destination has no parent."),
                    destination);
                if (!string.Equals(HashFile(destination), entry.BeforeHash,
                        StringComparison.Ordinal))
                {
                    return AtomicMutationResult.ConflictPreserved;
                }
                File.Delete(displaced);
                DeletePreparedOwnership(transactionRoot, incoming, owned);
                return AtomicMutationResult.Success;
            }
            if (File.Exists(destination))
            {
                EnsureRegularFile(
                    Path.GetDirectoryName(destination)
                    ?? throw new IOException(
                        "Seed destination has no parent."),
                    destination);
                if (File.Exists(owned) &&
                    SameFileObject(destination, owned) &&
                    string.Equals(HashFile(destination), entry.AfterHash,
                        StringComparison.Ordinal))
                {
                    File.Delete(displaced);
                    return AtomicMutationResult.Success;
                }
            }
            return AtomicMutationResult.ConflictPreserved;
        }
        if (!File.Exists(destination))
        {
            if (Directory.Exists(destination))
            {
                return AtomicMutationResult.ConflictPreserved;
            }
            if (File.Exists(incoming))
            {
                lease.MoveFile(displaced, destination, overwrite: false);
                DeletePreparedOwnership(transactionRoot, incoming, owned);
                return AtomicMutationResult.Success;
            }
            // Installation completed and the user later removed the owned
            // live object. Preserve that deletion.
            File.Delete(displaced);
            DeleteMutationFile(transactionRoot, owned);
            return AtomicMutationResult.ConflictRestored;
        }
        EnsureRegularFile(
            Path.GetDirectoryName(destination)
            ?? throw new IOException("Seed destination has no parent."),
            destination);
        string liveHash = HashFile(destination);
        if (string.Equals(liveHash, entry.BeforeHash,
                StringComparison.Ordinal))
        {
            File.Delete(displaced);
            DeletePreparedOwnership(transactionRoot, incoming, owned);
            return AtomicMutationResult.Success;
        }
        if (File.Exists(owned) &&
            SameFileObject(destination, owned) &&
            string.Equals(liveHash, entry.AfterHash,
                StringComparison.Ordinal))
        {
            // Normal rollback below atomically displaces the applied object
            // before restoring the immutable backup.
            File.Delete(displaced);
            return AtomicMutationResult.Success;
        }
        // A later user edit wins. The immutable backup already holds the same
        // before-image, so this redundant apply displacement can be removed.
        File.Delete(displaced);
        DeletePreparedOwnership(transactionRoot, incoming, owned);
        return AtomicMutationResult.ConflictRestored;
    }

    private static void PrepareIncoming(
        string source, string incoming, string expectedHash)
    {
        string quarantineRoot = Path.GetDirectoryName(incoming)
            ?? throw new IOException("Seed quarantine path has no parent.");
        EnsureDirectoryTree(quarantineRoot, create: false);
        using DirectoryMutationLease lease = DirectoryMutationLease.Acquire(
            quarantineRoot,
            Path.GetDirectoryName(source)
            ?? throw new IOException("Seed source has no parent."));
        if (File.Exists(incoming))
        {
            EnsureRegularFile(quarantineRoot, incoming);
            if (string.Equals(HashFile(incoming), expectedHash,
                    StringComparison.Ordinal))
            {
                return;
            }
            File.Delete(incoming);
        }
        using (var input = new FileStream(source, FileMode.Open, FileAccess.Read,
                   FileShare.Read))
        using (var output = new FileStream(
                   incoming,
                   FileMode.CreateNew,
                   FileAccess.Write,
                   FileShare.None,
                   bufferSize: 128 * 1024,
                   options: FileOptions.SequentialScan |
                            FileOptions.WriteThrough))
        {
            BeforeDurableArtifactWriteForTests?.Invoke(incoming);
            input.CopyTo(output);
            BeforeDurableArtifactFlushForTests?.Invoke(incoming);
            output.Flush(flushToDisk: true);
        }
        if (!string.Equals(HashFile(incoming), expectedHash,
                StringComparison.Ordinal))
        {
            File.Delete(incoming);
            throw new InvalidDataException(
                "Seed transaction source changed or failed hash verification.");
        }
    }

    private static string MutationPath(
        string transactionRoot, int index, string operation, string kind)
    {
        string quarantineRoot = Path.Combine(transactionRoot, "quarantine");
        EnsureDirectoryTree(quarantineRoot, create: false);
        return Path.Combine(quarantineRoot,
            $"{index:D8}.{operation}.{kind}");
    }

    private static bool MutationFileExists(
        string transactionRoot, int index, string operation, string kind)
    {
        string path = MutationPath(
            transactionRoot, index, operation, kind);
        if (Directory.Exists(path))
        {
            throw new InvalidDataException(
                "Seed mutation artifact has an invalid object type.");
        }
        return File.Exists(path);
    }

    private static void CreateOwnershipAnchor(string incoming, string owned)
    {
        EnsureNoReparseChain(incoming, includeLeaf: true);
        EnsureNoReparseChain(owned, includeLeaf: false);
        if (File.Exists(owned) || Directory.Exists(owned))
        {
            throw new IOException(
                "Seed ownership anchor already has an unexpected object.");
        }
        if (!NativeMethods.CreateHardLink(
                owned, incoming, IntPtr.Zero))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(),
                "Cannot create the seed ownership anchor.");
        }
        if (!SameFileObject(incoming, owned))
        {
            throw new IOException(
                "Seed ownership anchor identity verification failed.");
        }
    }

    private static bool SameFileObject(string left, string right) =>
        GetFileObjectIdentity(left) == GetFileObjectIdentity(right);

    private static FileObjectIdentity GetFileObjectIdentity(string path)
    {
        using SafeFileHandle handle = File.OpenHandle(
            path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.ReadWrite | FileShare.Delete);
        return GetFileObjectIdentity(handle);
    }

    private static FileObjectIdentity GetFileObjectIdentity(
        SafeFileHandle handle)
    {
        if (!NativeMethods.GetFileInformationByHandle(
                handle, out ByHandleFileInformation information))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(),
                "Cannot read seed file identity.");
        }
        ulong index = ((ulong)information.FileIndexHigh << 32) |
            information.FileIndexLow;
        if (index == 0)
        {
            throw new IOException("Seed file identity is unavailable.");
        }
        return new FileObjectIdentity(
            information.VolumeSerialNumber, index);
    }

    private static void DeletePreparedOwnership(
        string transactionRoot, string incoming, string owned)
    {
        DeleteMutationFile(transactionRoot, incoming);
        DeleteMutationFile(transactionRoot, owned);
    }

    private static bool HasMutationArtifacts(string transactionRoot)
    {
        string quarantineRoot = Path.Combine(transactionRoot, "quarantine");
        EnsureDirectoryTree(quarantineRoot, create: false);
        return Directory.EnumerateFileSystemEntries(quarantineRoot).Any();
    }

    private static bool HasUnresolvedMutationArtifacts(
        string transactionRoot, Receipt receipt)
    {
        string quarantineRoot = Path.Combine(transactionRoot, "quarantine");
        EnsureDirectoryTree(quarantineRoot, create: false);
        var expectedOwnership = new HashSet<string>(
            Enumerable.Range(0, receipt.Entries.Count).Select(index =>
                $"{index:D8}.apply.owned"),
            StringComparer.OrdinalIgnoreCase);
        var seenOwnership = new HashSet<string>(
            StringComparer.OrdinalIgnoreCase);
        foreach (string artifact in
                 Directory.EnumerateFileSystemEntries(quarantineRoot))
        {
            string leaf = Path.GetFileName(artifact);
            if (!expectedOwnership.Contains(leaf) ||
                !seenOwnership.Add(leaf))
            {
                return true;
            }
            EnsureRegularFile(transactionRoot, artifact);
        }
        return seenOwnership.Count != expectedOwnership.Count;
    }

    private static void DeleteMutationFile(string transactionRoot, string path)
    {
        if (!File.Exists(path))
        {
            return;
        }
        EnsureRegularFile(transactionRoot, path);
        File.Delete(path);
    }

    private static bool RestoreDisplaced(
        string destination, string displaced, DirectoryMutationLease lease)
    {
        if (!File.Exists(displaced) || File.Exists(destination) ||
            Directory.Exists(destination))
        {
            return false;
        }
        try
        {
            lease.MoveFile(displaced, destination, overwrite: false);
            return true;
        }
        catch (IOException)
        {
            return false;
        }
    }

    private static void WriteReceipt(string path, Receipt receipt)
    {
        receipt.Digest = ComputeDigest(receipt);
        byte[] json = JsonSerializer.SerializeToUtf8Bytes(receipt);
        if (json.Length is 0 or > MaxReceiptBytes)
        {
            throw new InvalidDataException("Seed receipt size is invalid.");
        }
        WriteDurableAtomic(path, json, overwrite: false);
    }

    private static void WriteRolledBackTombstone(
        string root,
        string transactionId,
        string expectedReceiptHash)
    {
        string source = Path.Combine(
            TransactionRoot(root, transactionId), ReceiptName);
        byte[] receiptBytes = ReadBoundedFile(source, MaxReceiptBytes);
        string actualHash =
            Convert.ToHexString(SHA256.HashData(receiptBytes));
        if (!string.Equals(
                actualHash,
                expectedReceiptHash,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "Seed rollback receipt changed before finalization.");
        }
        _ = ReadReceipt(receiptBytes, transactionId);
        WriteDurableAtomic(
            RolledBackTombstonePath(root, transactionId),
            receiptBytes,
            overwrite: false);
    }

    private static bool TryReadRolledBackTombstone(
        string root,
        string transactionId,
        string expectedReceiptHash)
    {
        ValidateTransactionId(transactionId);
        ValidateHash(expectedReceiptHash);
        string path = RolledBackTombstonePath(root, transactionId);
        if (!File.Exists(path))
        {
            return false;
        }
        string transactionsRoot = Path.Combine(root, ".transactions");
        EnsureDirectoryTree(root, create: false);
        EnsureDirectoryTree(transactionsRoot, create: false);
        EnsureRegularFile(transactionsRoot, path);
        byte[] receiptBytes = ReadBoundedFile(path, MaxReceiptBytes);
        string actualHash =
            Convert.ToHexString(SHA256.HashData(receiptBytes));
        if (!string.Equals(
                actualHash,
                expectedReceiptHash,
                StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "Seed rollback tombstone does not match the elevated journal hash.");
        }
        _ = ReadReceipt(receiptBytes, transactionId);
        return true;
    }

    private static Receipt ReadReceipt(byte[] json, string expectedTransactionId)
    {
        if (json.Length is 0 or > MaxReceiptBytes)
        {
            throw new InvalidDataException("Seed receipt size is invalid.");
        }
        Receipt receipt = JsonSerializer.Deserialize<Receipt>(json)
            ?? throw new InvalidDataException("Seed transaction receipt is empty.");
        if (receipt.Schema != Schema ||
            receipt.TransactionId != expectedTransactionId ||
            receipt.Phase != "Prepared" ||
            receipt.Entries is null ||
            receipt.Entries.Count > MaxEntries ||
            !string.Equals(receipt.Digest, ComputeDigest(receipt),
                StringComparison.Ordinal))
        {
            throw new InvalidDataException("Seed transaction receipt is invalid.");
        }
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        for (int index = 0; index < receipt.Entries.Count; index++)
        {
            Entry entry = receipt.Entries[index];
            string canonicalRelative = NormalizeRelative(entry.RelativePath);
            string expectedBackup = entry.Existed ? $"{index:D8}.bin" : "";
            if (entry.RelativePath != canonicalRelative ||
                !seen.Add(canonicalRelative) ||
                !IsUpperHash(entry.AfterHash) ||
                (entry.Existed &&
                 (!IsUpperHash(entry.BeforeHash) ||
                  !IsUpperHash(entry.BackupHash) ||
                  !string.Equals(entry.BackupHash, entry.BeforeHash,
                      StringComparison.Ordinal) ||
                  entry.BackupName != expectedBackup)) ||
                (!entry.Existed &&
                 (entry.BeforeHash.Length != 0 ||
                  entry.BackupHash.Length != 0 ||
                  entry.BackupName.Length != 0)))
            {
                throw new InvalidDataException(
                    "Seed transaction receipt entry is invalid.");
            }
        }
        return receipt;
    }

    private static byte[] ReadBoundedFile(string path, int maximumBytes)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read,
            FileShare.Read);
        if (stream.Length <= 0 || stream.Length > maximumBytes)
        {
            throw new InvalidDataException("Seed transaction file size is invalid.");
        }
        byte[] bytes = new byte[checked((int)stream.Length)];
        stream.ReadExactly(bytes);
        return bytes;
    }

    private static void DeleteEmptyParents(string root, string deletedFile)
    {
        string fullRoot = Path.GetFullPath(root).TrimEnd(
            Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        string? directory = Path.GetDirectoryName(deletedFile);
        while (directory is not null &&
               !string.Equals(directory, fullRoot,
                   StringComparison.OrdinalIgnoreCase))
        {
            _ = ContainedPath(fullRoot, directory);
            EnsureDirectoryTree(directory, create: false);
            if (Directory.EnumerateFileSystemEntries(directory).Any())
            {
                break;
            }
            Directory.Delete(directory);
            directory = Path.GetDirectoryName(directory);
        }
    }

    private static bool IsUpperHash(string value) =>
        value.Length == 64 &&
        value.All(c => (c >= '0' && c <= '9') ||
                       (c >= 'A' && c <= 'F'));

    private static string ComputeDigest(Receipt receipt)
    {
        var canonical = new StringBuilder();
        Append("schema", receipt.Schema);
        Append("transaction", receipt.TransactionId);
        Append("phase", receipt.Phase);
        foreach (Entry entry in receipt.Entries)
        {
            Append("path", entry.RelativePath);
            Append("existed", entry.Existed ? "1" : "0");
            Append("before", entry.BeforeHash);
            Append("backup_name", entry.BackupName);
            Append("backup_hash", entry.BackupHash);
            Append("after", entry.AfterHash);
        }
        return Convert.ToHexString(SHA256.HashData(
            Encoding.UTF8.GetBytes(canonical.ToString())));

        void Append(string name, string value) =>
            canonical.Append(name).Append(':').Append(value.Length).Append(':')
                .Append(value).Append(';');
    }

    private static void DeleteTransactionTree(string root, string transactionRoot)
    {
        _ = ContainedPath(Path.Combine(root, ".transactions"), transactionRoot);
        EnsureDirectoryTree(transactionRoot, create: false);
        _ = EnumerateRegularFiles(transactionRoot).ToArray();
        Directory.Delete(transactionRoot, recursive: true);
        string parent = Path.GetDirectoryName(transactionRoot)
            ?? throw new IOException("Seed transaction root has no parent.");
        EnsureDirectoryTree(parent, create: false);
        if (!Directory.EnumerateFileSystemEntries(parent).Any())
        {
            Directory.Delete(parent);
        }
    }

    private sealed class DirectoryMutationLease : IDisposable
    {
        private const uint FileReadAttributes = 0x80;
        private const uint DeleteAccess = 0x00010000;
        private const uint FileTraverse = 0x20;
        private const uint OpenExisting = 3;
        private const uint FileAttributeDirectory = 0x10;
        private const uint FileAttributeReparsePoint = 0x400;
        private const uint FileFlagOpenReparsePoint = 0x00200000;
        private const uint FileFlagBackupSemantics = 0x02000000;
        private readonly List<SafeFileHandle> _handles = [];
        private readonly Dictionary<string, SafeFileHandle> _directoryHandles =
            new(StringComparer.OrdinalIgnoreCase);

        private DirectoryMutationLease()
        {
        }

        internal static DirectoryMutationLease Acquire(
            params string[] directories)
        {
            var lease = new DirectoryMutationLease();
            try
            {
                var components = new HashSet<string>(
                    StringComparer.OrdinalIgnoreCase);
                foreach (string directory in directories)
                {
                    string full = Path.GetFullPath(directory);
                    if (!Directory.Exists(full))
                    {
                        throw new DirectoryNotFoundException(full);
                    }
                    string? current = full;
                    while (!string.IsNullOrEmpty(current))
                    {
                        components.Add(Path.TrimEndingDirectorySeparator(current));
                        string? parent = Path.GetDirectoryName(current);
                        if (string.IsNullOrEmpty(parent) ||
                            string.Equals(parent, current,
                                StringComparison.OrdinalIgnoreCase))
                        {
                            break;
                        }
                        current = parent;
                    }
                    string? volumeRoot = Path.GetPathRoot(full);
                    if (!string.IsNullOrEmpty(volumeRoot))
                    {
                        components.Add(Path.TrimEndingDirectorySeparator(
                            volumeRoot));
                    }
                }

                foreach (string component in components
                    .OrderBy(path => path.Length)
                    .ThenBy(path => path, StringComparer.OrdinalIgnoreCase))
                {
                    lease.OpenAndPin(component);
                }
                return lease;
            }
            catch
            {
                lease.Dispose();
                throw;
            }
        }

        private void OpenAndPin(string directory)
        {
            SafeFileHandle handle = NativeMethods.CreateFile(
                directory,
                FileReadAttributes | FileTraverse,
                FileShare.Read | FileShare.Write,
                IntPtr.Zero,
                OpenExisting,
                FileFlagBackupSemantics | FileFlagOpenReparsePoint,
                IntPtr.Zero);
            if (handle.IsInvalid)
            {
                int error = Marshal.GetLastWin32Error();
                handle.Dispose();
                throw new Win32Exception(
                    error, $"Cannot pin seed directory: {directory}");
            }
            try
            {
                if (!NativeMethods.GetFileInformationByHandle(
                        handle, out ByHandleFileInformation information) ||
                    (information.FileAttributes & FileAttributeDirectory) == 0 ||
                    (information.FileAttributes & FileAttributeReparsePoint) != 0)
                {
                    throw new IOException(
                        $"Unsafe seed directory component: {directory}");
                }
                ulong index = ((ulong)information.FileIndexHigh << 32) |
                    information.FileIndexLow;
                if (index == 0)
                {
                    throw new IOException(
                        $"Seed directory identity is unavailable: {directory}");
                }

                var finalPath = new StringBuilder(32_768);
                uint length = NativeMethods.GetFinalPathNameByHandle(
                    handle, finalPath, (uint)finalPath.Capacity, 0);
                if (length == 0 || length >= finalPath.Capacity)
                {
                    throw new Win32Exception(
                        Marshal.GetLastWin32Error(),
                        $"Cannot resolve seed directory: {directory}");
                }
                string resolved = NormalizeFinalPath(finalPath.ToString());
                string expected = Path.TrimEndingDirectorySeparator(
                    Path.GetFullPath(directory));
                if (!string.Equals(
                        resolved, expected, StringComparison.OrdinalIgnoreCase))
                {
                    throw new IOException(
                        $"Seed directory resolves outside its logical path: {directory}");
                }
                _handles.Add(handle);
                _directoryHandles.Add(expected, handle);
            }
            catch
            {
                handle.Dispose();
                throw;
            }
        }

        internal PreparedFileMove PrepareMove(
            string source, string destination, bool overwrite)
        {
            string sourceParent = NormalizeDirectory(
                Path.GetDirectoryName(source)
                ?? throw new IOException("Seed move source has no parent."));
            string destinationParent = NormalizeDirectory(
                Path.GetDirectoryName(destination)
                ?? throw new IOException(
                    "Seed move destination has no parent."));
            if (!_directoryHandles.ContainsKey(sourceParent) ||
                !_directoryHandles.TryGetValue(
                    destinationParent, out SafeFileHandle? parentHandle))
            {
                throw new IOException(
                    "Seed move escaped its pinned directory lease.");
            }
            string leaf = Path.GetFileName(destination);
            if (string.IsNullOrEmpty(leaf) ||
                leaf.IndexOfAny(
                    [Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar]) >= 0)
            {
                throw new IOException("Seed move destination leaf is invalid.");
            }

            SafeFileHandle sourceHandle = NativeMethods.CreateFile(
                source,
                DeleteAccess | FileReadAttributes,
                FileShare.Read | FileShare.Write,
                IntPtr.Zero,
                OpenExisting,
                FileFlagOpenReparsePoint,
                IntPtr.Zero);
            if (sourceHandle.IsInvalid)
            {
                int error = Marshal.GetLastWin32Error();
                sourceHandle.Dispose();
                throw new IOException(
                    $"Cannot pin seed move source: {source}",
                    new Win32Exception(error));
            }
            try
            {
                if (!NativeMethods.GetFileInformationByHandle(
                        sourceHandle, out ByHandleFileInformation information) ||
                    (information.FileAttributes &
                     (FileAttributeDirectory | FileAttributeReparsePoint)) != 0)
                {
                    throw new IOException(
                        $"Unsafe seed move source: {source}");
                }
                return new PreparedFileMove(
                    sourceHandle, parentHandle, leaf, overwrite);
            }
            catch
            {
                sourceHandle.Dispose();
                throw;
            }
        }

        internal void MoveFile(
            string source, string destination, bool overwrite)
        {
            using PreparedFileMove move =
                PrepareMove(source, destination, overwrite);
            move.Execute();
        }

        internal void MovePinnedFile(
            SafeFileHandle sourceHandle,
            string expectedSource,
            string destination,
            bool overwrite)
        {
            ArgumentNullException.ThrowIfNull(sourceHandle);
            if (sourceHandle.IsInvalid || sourceHandle.IsClosed)
            {
                throw new IOException(
                    "Pinned seed move source handle is unavailable.");
            }
            string sourceParent = NormalizeDirectory(
                Path.GetDirectoryName(expectedSource)
                ?? throw new IOException("Seed move source has no parent."));
            string destinationParent = NormalizeDirectory(
                Path.GetDirectoryName(destination)
                ?? throw new IOException(
                    "Seed move destination has no parent."));
            if (!_directoryHandles.ContainsKey(sourceParent) ||
                !_directoryHandles.TryGetValue(
                    destinationParent, out SafeFileHandle? parentHandle))
            {
                throw new IOException(
                    "Seed move escaped its pinned directory lease.");
            }
            if (!NativeMethods.GetFileInformationByHandle(
                    sourceHandle, out ByHandleFileInformation information) ||
                (information.FileAttributes &
                 (FileAttributeDirectory | FileAttributeReparsePoint)) != 0)
            {
                throw new IOException("Unsafe pinned seed move source.");
            }
            var finalPath = new StringBuilder(32_768);
            uint length = NativeMethods.GetFinalPathNameByHandle(
                sourceHandle, finalPath, (uint)finalPath.Capacity, 0);
            if (length == 0 || length >= finalPath.Capacity ||
                !string.Equals(
                    NormalizeFinalPath(finalPath.ToString()),
                    Path.GetFullPath(expectedSource),
                    StringComparison.OrdinalIgnoreCase))
            {
                throw new IOException(
                    "Pinned seed move source identity changed.");
            }
            string leaf = Path.GetFileName(destination);
            if (string.IsNullOrEmpty(leaf) ||
                leaf.IndexOfAny(
                    [Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar]) >= 0)
            {
                throw new IOException("Seed move destination leaf is invalid.");
            }
            using var move = new PreparedFileMove(
                sourceHandle,
                parentHandle,
                leaf,
                overwrite,
                ownsSource: false,
                clearDeleteDispositionBeforeExecute: true);
            move.Execute();
        }

        private static string NormalizeDirectory(string directory) =>
            Path.TrimEndingDirectorySeparator(Path.GetFullPath(directory));

        private static string NormalizeFinalPath(string path)
        {
            if (path.StartsWith(
                    @"\\?\UNC\", StringComparison.OrdinalIgnoreCase))
            {
                path = @"\\" + path[8..];
            }
            else if (path.StartsWith(
                         @"\\?\", StringComparison.OrdinalIgnoreCase))
            {
                path = path[4..];
            }
            return Path.TrimEndingDirectorySeparator(
                Path.GetFullPath(path));
        }

        public void Dispose()
        {
            for (int index = _handles.Count - 1; index >= 0; index--)
            {
                _handles[index].Dispose();
            }
            _handles.Clear();
            _directoryHandles.Clear();
        }

        internal sealed class PreparedFileMove : IDisposable
        {
            private const int FileRenameInformation = 10;
            private readonly SafeFileHandle _source;
            private readonly SafeFileHandle _destinationParent;
            private readonly string _destinationLeaf;
            private readonly bool _overwrite;
            private readonly bool _ownsSource;
            private readonly bool _clearDeleteDispositionBeforeExecute;
            private bool _executed;

            internal PreparedFileMove(
                SafeFileHandle source,
                SafeFileHandle destinationParent,
                string destinationLeaf,
                bool overwrite,
                bool ownsSource = true,
                bool clearDeleteDispositionBeforeExecute = false)
            {
                _source = source;
                _destinationParent = destinationParent;
                _destinationLeaf = destinationLeaf;
                _overwrite = overwrite;
                _ownsSource = ownsSource;
                _clearDeleteDispositionBeforeExecute =
                    clearDeleteDispositionBeforeExecute;
            }

            internal void Execute()
            {
                if (_executed)
                {
                    throw new InvalidOperationException(
                        "Seed move was already executed.");
                }
                _executed = true;
                byte[] name = Encoding.Unicode.GetBytes(_destinationLeaf);
                int rootOffset = IntPtr.Size == 8 ? 8 : 4;
                int nameLengthOffset = rootOffset + IntPtr.Size;
                int nameOffset = nameLengthOffset + sizeof(uint);
                IntPtr information = Marshal.AllocHGlobal(
                    checked(nameOffset + name.Length));
                try
                {
                    Marshal.Copy(
                        new byte[nameOffset + name.Length],
                        0,
                        information,
                        nameOffset + name.Length);
                    Marshal.WriteByte(
                        information, 0, _overwrite ? (byte)1 : (byte)0);
                    Marshal.WriteIntPtr(
                        information,
                        rootOffset,
                        _destinationParent.DangerousGetHandle());
                    Marshal.WriteInt32(
                        information, nameLengthOffset, name.Length);
                    Marshal.Copy(name, 0, information + nameOffset, name.Length);
                    if (_clearDeleteDispositionBeforeExecute)
                    {
                        SetDeleteDisposition(
                            _source,
                            delete: false,
                            "Cannot publish the pinned seed temporary file.");
                        AfterPinnedTemporaryDeleteDispositionClearedForTests
                            ?.Invoke();
                    }
                    int status = NativeMethods.NtSetInformationFile(
                            _source,
                            out IoStatusBlock _,
                            information,
                            (uint)(nameOffset + name.Length),
                            FileRenameInformation);
                    if (status < 0)
                    {
                        if (_clearDeleteDispositionBeforeExecute)
                        {
                            try
                            {
                                SetDeleteDisposition(
                                    _source,
                                    delete: true,
                                    "Cannot re-arm seed temporary cleanup.");
                            }
                            catch
                            {
                                // Preserve the original atomic rename error.
                            }
                        }
                        throw new IOException(
                            "Cannot atomically rename a pinned seed file.",
                            new Win32Exception((int)
                                NativeMethods.RtlNtStatusToDosError(status)));
                    }
                    if (_clearDeleteDispositionBeforeExecute)
                    {
                        // The recovery record can live on a different volume.
                        // Do not remove it until the destination volume has
                        // persisted the rename metadata.
                        AfterPinnedAtomicFileRenameForTests?.Invoke();
                        FlushPinnedFile(
                            _source,
                            "Cannot flush the published seed file.");
                    }
                }
                finally
                {
                    Marshal.FreeHGlobal(information);
                }
            }

            public void Dispose()
            {
                if (_ownsSource)
                {
                    _source.Dispose();
                }
            }
        }
    }

    private sealed class AtomicPublicationRecoveryLease(
        FileStream stream) : IDisposable
    {
        private bool _deleteArmed;

        internal void DeleteOnClose()
        {
            if (_deleteArmed)
            {
                throw new InvalidOperationException(
                    "Atomic publication recovery was already completed.");
            }
            SetDeleteDisposition(
                stream.SafeFileHandle,
                delete: true,
                "Cannot remove the completed atomic publication recovery record.");
            _deleteArmed = true;
        }

        public void Dispose() => stream.Dispose();
    }

    private sealed class AtomicPublicationRecoveryRecord
    {
        public string Schema { get; set; } = "";
        public string Temporary { get; set; } = "";
        public string Destination { get; set; } = "";
        public uint VolumeSerialNumber { get; set; }
        public ulong FileIndex { get; set; }
    }

    private sealed class Receipt
    {
        public string Schema { get; set; } = "";
        public string TransactionId { get; set; } = "";
        public string Phase { get; set; } = "";
        public List<Entry> Entries { get; set; } = [];
        public string Digest { get; set; } = "";
    }

    private sealed class Entry
    {
        public string RelativePath { get; set; } = "";
        public bool Existed { get; set; }
        public string BeforeHash { get; set; } = "";
        public string BackupName { get; set; } = "";
        public string BackupHash { get; set; } = "";
        public string AfterHash { get; set; } = "";
    }

    private enum AtomicMutationResult
    {
        Success,
        ConflictRestored,
        ConflictPreserved,
    }

    private readonly record struct FileObjectIdentity(
        uint VolumeSerialNumber, ulong FileIndex);

    private const int ErrorFileNotFound = 2;
    private const int ErrorPathNotFound = 3;
    private const uint GenericRead = 0x80000000;
    private const uint GenericWrite = 0x40000000;
    private const uint DeleteAccess = 0x00010000;
    private const uint FileReadAttributes = 0x00000080;
    private const uint CreateNew = 1;
    private const uint OpenExisting = 3;
    private const uint FileAttributeDirectory = 0x00000010;
    private const uint FileAttributeNormal = 0x00000080;
    private const uint FileAttributeTemporary = 0x00000100;
    private const uint FileAttributeReparsePoint = 0x00000400;
    private const uint FileFlagSequentialScan = 0x08000000;
    private const uint FileFlagWriteThrough = 0x80000000;
    private const uint FileFlagOpenReparsePoint = 0x00200000;
    private const int FileDispositionInfo = 4;

    [StructLayout(LayoutKind.Sequential)]
    private struct FileDispositionInformation
    {
        [MarshalAs(UnmanagedType.Bool)]
        internal bool DeleteFile;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ByHandleFileInformation
    {
        internal uint FileAttributes;
        internal System.Runtime.InteropServices.ComTypes.FILETIME CreationTime;
        internal System.Runtime.InteropServices.ComTypes.FILETIME LastAccessTime;
        internal System.Runtime.InteropServices.ComTypes.FILETIME LastWriteTime;
        internal uint VolumeSerialNumber;
        internal uint FileSizeHigh;
        internal uint FileSizeLow;
        internal uint NumberOfLinks;
        internal uint FileIndexHigh;
        internal uint FileIndexLow;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct IoStatusBlock
    {
        internal IntPtr Status;
        internal IntPtr Information;
    }

    private static class NativeMethods
    {
        [DllImport("kernel32.dll", EntryPoint = "CreateFileW",
            SetLastError = true, CharSet = CharSet.Unicode)]
        internal static extern SafeFileHandle CreateFile(
            string fileName,
            uint desiredAccess,
            FileShare shareMode,
            IntPtr securityAttributes,
            uint creationDisposition,
            uint flagsAndAttributes,
            IntPtr templateFile);

        [DllImport("kernel32.dll", EntryPoint = "GetFinalPathNameByHandleW",
            SetLastError = true, CharSet = CharSet.Unicode)]
        internal static extern uint GetFinalPathNameByHandle(
            SafeFileHandle file,
            StringBuilder path,
            uint pathLength,
            uint flags);

        [DllImport("kernel32.dll", EntryPoint = "CreateHardLinkW",
            SetLastError = true, CharSet = CharSet.Unicode)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool CreateHardLink(
            string newFileName, string existingFileName,
            IntPtr securityAttributes);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool GetFileInformationByHandle(
            SafeFileHandle file,
            out ByHandleFileInformation information);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool FlushFileBuffers(SafeFileHandle file);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool SetFileInformationByHandle(
            SafeFileHandle file,
            int fileInformationClass,
            ref FileDispositionInformation fileInformation,
            uint bufferSize);

        [DllImport("ntdll.dll")]
        internal static extern int NtSetInformationFile(
            SafeFileHandle file,
            out IoStatusBlock ioStatusBlock,
            IntPtr fileInformation,
            uint bufferSize,
            int fileInformationClass);

        [DllImport("ntdll.dll")]
        internal static extern uint RtlNtStatusToDosError(int status);
    }
}
