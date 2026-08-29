using System.Runtime.InteropServices;

namespace ExplorerRemoteFs.Config;

/// <summary>
/// Windows Credential Manager 读取（CredRead）。
/// 站点密码由 GUI 客户端写入（目标名 "ExplorerRemoteFs/&lt;站点名&gt;"，DPAPI 保护），
/// 本类在连接建立前把密码解出来填回 ConnectionConfig。
/// </summary>
public static class CredentialManager
{
    private const int CRED_TYPE_GENERIC = 1;
    private const string Prefix = "ExplorerRemoteFs/";

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
    private static extern bool CredRead(string target, int type, int flags, out IntPtr credPtr);

    [DllImport("advapi32.dll")]
    private static extern void CredFree(IntPtr cred);

    /// <summary>按站点名读取已保存的密码；未保存返回 false。</summary>
    public static bool TryRead(string siteName, out string userName, out string secret)
    {
        userName = "";
        secret = "";
        if (!CredRead(Prefix + siteName, CRED_TYPE_GENERIC, 0, out IntPtr ptr)) return false;
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
        finally
        {
            CredFree(ptr);
        }
    }
}
