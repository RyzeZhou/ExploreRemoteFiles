using System.Runtime.InteropServices;

namespace RemoteFsClient.Services;

/// <summary>
/// Windows 凭据管理器封装（CredWrite/CredRead/CredDelete）。
/// 每个站点的密码存为 Generic 凭据，目标名 "ExplorerRemoteFs/&lt;站点名&gt;"，
/// Credential Blob 为 DPAPI 保护的 Unicode 字节，仅当前 Windows 账户可解。
/// 密码永远不写进 connections.json。
/// </summary>
public static class CredentialStore
{
    private const int CRED_TYPE_GENERIC = 1;
    private const string Prefix = "ExplorerRemoteFs/";

    public static string TargetFor(string siteName) => Prefix + siteName;

    // ---- public API -------------------------------------------------------

    public static void Write(string siteName, string userName, string secret)
    {
        var target = TargetFor(siteName);
        var cred = new CREDENTIAL
        {
            Flags = 0,
            Type = CRED_TYPE_GENERIC,
            TargetName = Marshal.StringToHGlobalUni(target),
            Comment = Marshal.StringToHGlobalUni("RemoteFsClient site credential"),
            Persist = 2, // CRED_PERSIST_LOCAL_MACHINE
            AttributeCount = 0,
            Attributes = IntPtr.Zero,
            TargetAlias = IntPtr.Zero,
            UserName = Marshal.StringToHGlobalUni(userName),
        };
        try
        {
            byte[] blob = System.Text.Encoding.Unicode.GetBytes(secret);
            cred.CredentialBlobSize = (uint)blob.Length;
            cred.CredentialBlob = Marshal.AllocHGlobal(blob.Length);
            Marshal.Copy(blob, 0, cred.CredentialBlob, blob.Length);
            if (!CredWrite(ref cred, 0))
                throw new InvalidOperationException($"CredWrite failed (Win32 error {Marshal.GetLastWin32Error()})");
        }
        finally
        {
            if (cred.CredentialBlob != IntPtr.Zero) Marshal.FreeHGlobal(cred.CredentialBlob);
            if (cred.TargetName != IntPtr.Zero) Marshal.FreeHGlobal(cred.TargetName);
            if (cred.Comment != IntPtr.Zero) Marshal.FreeHGlobal(cred.Comment);
            if (cred.UserName != IntPtr.Zero) Marshal.FreeHGlobal(cred.UserName);
        }
    }

    public static bool TryRead(string siteName, out string userName, out string secret)
    {
        userName = ""; secret = "";
        if (CredRead(TargetFor(siteName), CRED_TYPE_GENERIC, 0, out IntPtr ptr))
        {
            try
            {
                var cred = Marshal.PtrToStructure<CREDENTIAL>(ptr);
                if (cred.CredentialBlob != IntPtr.Zero && cred.CredentialBlobSize > 0)
                {
                    byte[] blob = new byte[cred.CredentialBlobSize];
                    Marshal.Copy(cred.CredentialBlob, blob, 0, blob.Length);
                    secret = System.Text.Encoding.Unicode.GetString(blob);
                }
                if (cred.UserName != IntPtr.Zero) userName = Marshal.PtrToStringUni(cred.UserName) ?? "";
                return secret.Length > 0;
            }
            finally { CredFree(ptr); }
        }
        return false;
    }

    public static bool Exists(string siteName) => TryRead(siteName, out _, out _);

    public static bool Delete(string siteName)
        => CredDelete(TargetFor(siteName), CRED_TYPE_GENERIC, 0);

    // ---- native -----------------------------------------------------------

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct CREDENTIAL
    {
        public uint Flags;
        public uint Type;
        public IntPtr TargetName;
        public IntPtr Comment;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastWritten;
        public uint CredentialBlobSize;
        public IntPtr CredentialBlob;
        public uint Persist;
        public uint AttributeCount;
        public IntPtr Attributes;
        public IntPtr TargetAlias;
        public IntPtr UserName;
    }

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CredWrite(ref CREDENTIAL cred, uint flags);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CredRead(string target, int type, int flags, out IntPtr credPtr);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CredDelete(string target, int type, int flags);

    [DllImport("advapi32.dll")]
    private static extern void CredFree(IntPtr cred);
}
