using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace Famo.Settings.Core.Ai;

/// <summary>Windows Credential Manager backed secret store for AI API keys.</summary>
public sealed class WindowsCredentialSecretStore : ISecretStore
{
    private const string TargetPrefix = "Famo.AI.";
    private const uint CredTypeGeneric = 1;
    private const uint CredPersistLocalMachine = 2;
    private const int ErrorNotFound = 1168;

    public void SetSecret(string name, string value)
    {
        EnsureWindows();
        ArgumentException.ThrowIfNullOrWhiteSpace(name);
        ArgumentException.ThrowIfNullOrWhiteSpace(value);

        byte[] bytes = Encoding.UTF8.GetBytes(value);
        IntPtr blob = Marshal.AllocCoTaskMem(bytes.Length);
        try
        {
            Marshal.Copy(bytes, 0, blob, bytes.Length);
            var credential = new NativeCredential
            {
                Type = CredTypeGeneric,
                TargetName = TargetName(name),
                CredentialBlobSize = (uint)bytes.Length,
                CredentialBlob = blob,
                Persist = CredPersistLocalMachine,
                UserName = Environment.UserName,
            };

            if (!CredWrite(ref credential, 0))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
        }
        finally
        {
            CryptographicOperations.ZeroMemory(bytes);
            if (blob != IntPtr.Zero)
            {
                Marshal.Copy(new byte[bytes.Length], 0, blob, bytes.Length);
                Marshal.FreeCoTaskMem(blob);
            }
        }
    }

    public string? GetSecret(string name)
    {
        EnsureWindows();
        ArgumentException.ThrowIfNullOrWhiteSpace(name);

        if (!CredRead(TargetName(name), CredTypeGeneric, 0, out IntPtr credentialPtr))
        {
            int error = Marshal.GetLastWin32Error();
            if (error == ErrorNotFound) return null;
            throw new Win32Exception(error);
        }

        try
        {
            NativeCredential credential = Marshal.PtrToStructure<NativeCredential>(credentialPtr);
            if (credential.CredentialBlob == IntPtr.Zero || credential.CredentialBlobSize == 0) return string.Empty;

            byte[] bytes = new byte[credential.CredentialBlobSize];
            Marshal.Copy(credential.CredentialBlob, bytes, 0, bytes.Length);
            try
            {
                return Encoding.UTF8.GetString(bytes);
            }
            finally
            {
                CryptographicOperations.ZeroMemory(bytes);
            }
        }
        finally
        {
            CredFree(credentialPtr);
        }
    }

    public void DeleteSecret(string name)
    {
        EnsureWindows();
        ArgumentException.ThrowIfNullOrWhiteSpace(name);

        if (!CredDelete(TargetName(name), CredTypeGeneric, 0))
        {
            int error = Marshal.GetLastWin32Error();
            if (error != ErrorNotFound)
            {
                throw new Win32Exception(error);
            }
        }
    }

    private static string TargetName(string name) => TargetPrefix + name;

    private static void EnsureWindows()
    {
        if (!OperatingSystem.IsWindows())
        {
            throw new PlatformNotSupportedException("AI API keys are stored in Windows Credential Manager.");
        }
    }

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CredWrite(ref NativeCredential credential, uint flags);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CredRead(string targetName, uint type, uint reservedFlag, out IntPtr credentialPtr);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CredDelete(string targetName, uint type, uint flags);

    [DllImport("advapi32.dll", SetLastError = false)]
    private static extern void CredFree(IntPtr buffer);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct NativeCredential
    {
        public uint Flags;
        public uint Type;
        public string TargetName;
        public string? Comment;
        public long LastWritten;
        public uint CredentialBlobSize;
        public IntPtr CredentialBlob;
        public uint Persist;
        public uint AttributeCount;
        public IntPtr Attributes;
        public string? TargetAlias;
        public string? UserName;
    }
}
