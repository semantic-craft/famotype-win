using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.ExceptionServices;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace Famo.Settings.Core;

internal static class UserDataTransactionLock
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(30);
    [ThreadStatic]
    private static Releaser? _threadLock;
    [ThreadStatic]
    private static int _threadLockDepth;
    internal static bool DisableNamedMutexForTests { get; set; }
    internal static Action<string>? AfterFileLockDisposeForTests { get; set; }
    internal static Action<string>? BeforeLockFileOpenForTests { get; set; }
    internal static string? LocalAppDataOverrideForTests { get; set; }

    internal static string GlobalMutexNameForTests =>
        TryGetGlobalMutexName()
        ?? throw new PlatformNotSupportedException(
            "The current Windows user SID is unavailable.");

    internal static string LockFilePathForTests(string dataRoot) =>
        GetLockFilePath(dataRoot);

    public static IDisposable Acquire(string? dataRoot = null)
    {
        string root = Path.GetFullPath(dataRoot ?? FamoPaths.FamoDir);
        if (_threadLock is not null)
        {
            _threadLockDepth++;
            return new RecursiveLease();
        }
        var elapsed = Stopwatch.StartNew();
        Mutex? gate = null;
        bool entered = false;
        try
        {
            if (!DisableNamedMutexForTests)
            {
                string? name = TryGetGlobalMutexName();
                if (name is not null)
                {
                    try
                    {
                        gate = new Mutex(initiallyOwned: false, name);
                    }
                    catch (Exception ex) when (
                        ex is UnauthorizedAccessException or IOException or
                        WaitHandleCannotBeOpenedException or
                        PlatformNotSupportedException)
                    {
                        // The file lock below is the mandatory cross-process
                        // fallback; never continue without one of the two.
                        gate = null;
                    }
                }
            }

            if (gate is not null)
            {
                try
                {
                    entered = gate.WaitOne(Remaining(elapsed));
                }
                catch (AbandonedMutexException)
                {
                    entered = true;
                }
                if (!entered)
                {
                    throw new TimeoutException(
                        "Timed out waiting for the global Famo user-data lock.");
                }
            }

            FileLockLease file = AcquireFileLock(root, elapsed);
            _threadLock = new Releaser(gate, entered, file);
            _threadLockDepth = 1;
            return new RecursiveLease();
        }
        catch (Exception failure)
        {
            Exception? cleanupFailure = null;
            try
            {
                if (entered)
                {
                    gate!.ReleaseMutex();
                }
            }
            catch (Exception ex)
            {
                cleanupFailure = ex;
            }
            try
            {
                gate?.Dispose();
            }
            catch (Exception ex)
            {
                cleanupFailure = cleanupFailure is null
                    ? ex
                    : new AggregateException(cleanupFailure, ex);
            }
            if (cleanupFailure is not null)
            {
                throw new AggregateException(
                    "Famo user-data lock acquisition and cleanup both failed.",
                    failure,
                    cleanupFailure);
            }
            ExceptionDispatchInfo.Capture(failure).Throw();
            throw;
        }
    }

    private sealed class RecursiveLease : IDisposable
    {
        private bool _released;

        public void Dispose()
        {
            if (_released)
            {
                return;
            }
            _released = true;
            if (_threadLock is null || _threadLockDepth <= 0)
            {
                throw new SynchronizationLockException(
                    "The Famo user-data lock was disposed on a different thread.");
            }
            _threadLockDepth--;
            if (_threadLockDepth != 0)
            {
                return;
            }
            Releaser held = _threadLock;
            _threadLock = null;
            held.Dispose();
        }
    }

    private static FileLockLease AcquireFileLock(
        string dataRoot, Stopwatch elapsed)
    {
        string path = GetLockFilePath(dataRoot);
        string directory = Path.GetDirectoryName(path)
            ?? throw new IOException("Famo lock path has no parent.");
        Directory.CreateDirectory(directory);
        PinnedLockDirectory pinned = PinnedLockDirectory.Open(directory);
        try
        {
            BeforeLockFileOpenForTests?.Invoke(directory);
            while (true)
            {
                int error = pinned.TryOpenLockFile(
                    Path.GetFileName(path), path,
                    out SafeFileHandle? file);
                if (error == 0)
                {
                    return new FileLockLease(pinned, file!, path);
                }
                if (error is not 32 and not 33)
                {
                    throw new IOException(
                        "Cannot open the Famo user-data file lock.",
                        new Win32Exception(error));
                }
                if (elapsed.Elapsed >= Timeout)
                {
                    throw new TimeoutException(
                        "Timed out waiting for the Famo user-data file lock.");
                }
                Thread.Sleep(25);
            }
        }
        catch
        {
            pinned.Dispose();
            throw;
        }
    }

    private static TimeSpan Remaining(Stopwatch elapsed)
    {
        TimeSpan remaining = Timeout - elapsed.Elapsed;
        return remaining > TimeSpan.Zero ? remaining : TimeSpan.Zero;
    }

    private static string? TryGetGlobalMutexName()
    {
        string? sid = TryGetCurrentUserSid();
        return sid is null
            ? null
            : $@"Global\Famo.Settings.UserData.Transaction.{sid}";
    }

    private static string? TryGetCurrentUserSid()
    {
        if (!OperatingSystem.IsWindows())
        {
            return null;
        }
        try
        {
            string? sid = WindowsIdentity.GetCurrent().User?.Value;
            return string.IsNullOrWhiteSpace(sid) ? null : sid;
        }
        catch (Exception ex) when (ex is SystemException)
        {
            return null;
        }
    }

    private static string GetLockFilePath(string dataRoot)
    {
        _ = dataRoot;
        string identity = TryGetCurrentUserSid()
            ?? (OperatingSystem.IsWindows()
                ? throw new PlatformNotSupportedException(
                    "The current Windows user SID is unavailable.")
                : Environment.UserName);
        if (identity.Any(character =>
                !(char.IsLetterOrDigit(character) ||
                  character is '-' or '_')))
        {
            throw new IOException("The Famo lock identity is unsafe.");
        }
        string localData = LocalAppDataOverrideForTests ??
            Environment.GetFolderPath(
                Environment.SpecialFolder.LocalApplicationData,
                Environment.SpecialFolderOption.DoNotVerify);
        if (string.IsNullOrWhiteSpace(localData))
        {
            throw new IOException(
                "The current user LocalAppData path is unavailable.");
        }
        localData = Path.GetFullPath(localData);
        return Path.Combine(
            localData,
            "Famo.UserDataLocks",
            $"{identity}.transaction.lock");
    }

    private sealed class FileLockLease(
        PinnedLockDirectory directory,
        SafeFileHandle file,
        string path) : IDisposable
    {
        internal string Path { get; } = path;

        public void Dispose()
        {
            file.Dispose();
            directory.Dispose();
        }
    }

    private sealed class PinnedLockDirectory : IDisposable
    {
        private const uint FileReadAttributes = 0x80;
        private const uint FileWriteData = 0x2;
        private const uint FileReadData = 0x1;
        private const uint FileTraverse = 0x20;
        private const uint Synchronize = 0x00100000;
        private const uint OpenExisting = 3;
        private const uint FileOpenIf = 3;
        private const uint FileAttributeDirectory = 0x10;
        private const uint FileAttributeNormal = 0x80;
        private const uint FileAttributeReparsePoint = 0x400;
        private const uint FileSynchronousIoNonAlert = 0x20;
        private const uint FileNonDirectoryFile = 0x40;
        private const uint FileOpenReparsePoint = 0x00200000;
        private const uint FileFlagOpenReparsePoint = 0x00200000;
        private const uint FileFlagBackupSemantics = 0x02000000;
        private const uint ObjCaseInsensitive = 0x40;
        private readonly SafeFileHandle _handle;

        private PinnedLockDirectory(SafeFileHandle handle) =>
            _handle = handle;

        internal static PinnedLockDirectory Open(string directory)
        {
            string expected = Path.TrimEndingDirectorySeparator(
                Path.GetFullPath(directory));
            SafeFileHandle handle = NativeMethods.CreateFile(
                expected,
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
                throw new IOException(
                    "Cannot pin the Famo user-data lock directory.",
                    new Win32Exception(error));
            }
            try
            {
                ValidatePinnedObject(
                    handle, expected, expectDirectory: true);
                return new PinnedLockDirectory(handle);
            }
            catch
            {
                handle.Dispose();
                throw;
            }
        }

        internal int TryOpenLockFile(
            string leaf, string expectedPath, out SafeFileHandle? file)
        {
            file = null;
            if (string.IsNullOrEmpty(leaf) ||
                leaf.IndexOfAny(
                    [Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar]) >= 0)
            {
                return 87;
            }
            IntPtr nameBuffer = Marshal.StringToHGlobalUni(leaf);
            IntPtr namePointer = IntPtr.Zero;
            try
            {
                var name = new UnicodeString
                {
                    Length = checked((ushort)(leaf.Length * sizeof(char))),
                    MaximumLength =
                        checked((ushort)(leaf.Length * sizeof(char))),
                    Buffer = nameBuffer,
                };
                namePointer = Marshal.AllocHGlobal(
                    Marshal.SizeOf<UnicodeString>());
                Marshal.StructureToPtr(name, namePointer, fDeleteOld: false);
                var attributes = new ObjectAttributes
                {
                    Length = Marshal.SizeOf<ObjectAttributes>(),
                    RootDirectory = _handle.DangerousGetHandle(),
                    ObjectName = namePointer,
                    Attributes = ObjCaseInsensitive,
                };
                int status = NativeMethods.NtCreateFile(
                    out SafeFileHandle opened,
                    FileReadData | FileWriteData |
                    FileReadAttributes | Synchronize,
                    ref attributes,
                    out IoStatusBlock _,
                    IntPtr.Zero,
                    FileAttributeNormal,
                    0,
                    FileOpenIf,
                    FileNonDirectoryFile |
                    FileSynchronousIoNonAlert |
                    FileOpenReparsePoint,
                    IntPtr.Zero,
                    0);
                if (status < 0)
                {
                    opened?.Dispose();
                    return checked((int)
                        NativeMethods.RtlNtStatusToDosError(status));
                }
                try
                {
                    ValidatePinnedObject(
                        opened,
                        Path.GetFullPath(expectedPath),
                        expectDirectory: false);
                    file = opened;
                    return 0;
                }
                catch
                {
                    opened.Dispose();
                    throw;
                }
            }
            finally
            {
                if (namePointer != IntPtr.Zero)
                {
                    Marshal.DestroyStructure<UnicodeString>(namePointer);
                    Marshal.FreeHGlobal(namePointer);
                }
                Marshal.FreeHGlobal(nameBuffer);
            }
        }

        private static void ValidatePinnedObject(
            SafeFileHandle handle, string expectedPath, bool expectDirectory)
        {
            if (!NativeMethods.GetFileInformationByHandle(
                    handle, out ByHandleFileInformation information))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            bool directory =
                (information.FileAttributes & FileAttributeDirectory) != 0;
            if (directory != expectDirectory ||
                (information.FileAttributes & FileAttributeReparsePoint) != 0)
            {
                throw new IOException(
                    "The Famo user-data lock object is unsafe.");
            }
            ulong objectId = ((ulong)information.FileIndexHigh << 32) |
                information.FileIndexLow;
            if (objectId == 0)
            {
                throw new IOException(
                    "The Famo user-data lock object identity is unavailable.");
            }
            var finalPath = new StringBuilder(32_768);
            uint length = NativeMethods.GetFinalPathNameByHandle(
                handle, finalPath, (uint)finalPath.Capacity, 0);
            if (length == 0 || length >= finalPath.Capacity)
            {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            string resolved = NormalizeFinalPath(finalPath.ToString());
            string expected = Path.TrimEndingDirectorySeparator(
                Path.GetFullPath(expectedPath));
            if (!string.Equals(
                    resolved, expected, StringComparison.OrdinalIgnoreCase))
            {
                throw new IOException(
                    $"The Famo user-data lock object escaped its path: " +
                    $"expected={expected}; resolved={resolved}");
            }
        }

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

        public void Dispose() => _handle.Dispose();
    }

    private sealed class Releaser(
        Mutex? gate, bool entered, FileLockLease file) : IDisposable
    {
        private bool _released;

        public void Dispose()
        {
            if (_released)
            {
                return;
            }
            _released = true;
            Exception? failure = null;
            try
            {
                try
                {
                    file.Dispose();
                    AfterFileLockDisposeForTests?.Invoke(file.Path);
                }
                catch (Exception ex)
                {
                    failure = ex;
                }
            }
            finally
            {
                try
                {
                    if (entered)
                    {
                        gate!.ReleaseMutex();
                    }
                }
                catch (Exception ex)
                {
                    failure = failure is null
                        ? ex
                        : new AggregateException(failure, ex);
                }
                finally
                {
                    try
                    {
                        gate?.Dispose();
                    }
                    catch (Exception ex)
                    {
                        failure = failure is null
                            ? ex
                            : new AggregateException(failure, ex);
                    }
                }
            }
            if (failure is not null)
            {
                ExceptionDispatchInfo.Capture(failure).Throw();
            }
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct UnicodeString
    {
        internal ushort Length;
        internal ushort MaximumLength;
        internal IntPtr Buffer;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ObjectAttributes
    {
        internal int Length;
        internal IntPtr RootDirectory;
        internal IntPtr ObjectName;
        internal uint Attributes;
        internal IntPtr SecurityDescriptor;
        internal IntPtr SecurityQualityOfService;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct IoStatusBlock
    {
        internal IntPtr Status;
        internal IntPtr Information;
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

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool GetFileInformationByHandle(
            SafeFileHandle file,
            out ByHandleFileInformation information);

        [DllImport("ntdll.dll")]
        internal static extern int NtCreateFile(
            out SafeFileHandle file,
            uint desiredAccess,
            ref ObjectAttributes objectAttributes,
            out IoStatusBlock ioStatusBlock,
            IntPtr allocationSize,
            uint fileAttributes,
            uint shareAccess,
            uint createDisposition,
            uint createOptions,
            IntPtr eaBuffer,
            uint eaLength);

        [DllImport("ntdll.dll")]
        internal static extern uint RtlNtStatusToDosError(int status);
    }
}
