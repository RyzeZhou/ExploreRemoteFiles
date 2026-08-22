using System;
using System.Runtime.InteropServices;

namespace ExplorerRemoteFs.Shell.Interop
{
    /// <summary>
    /// Shell32.dll / shlwapi.dll 的最小 P/Invoke 集（规范第 6 节）。
    /// </summary>
    public static class Shell32
    {
        /// <summary>
        /// 创建默认 Shell 文件夹视图（DefView）。推荐用于 namespace 扩展。
        /// Win10/11 兼容，推荐替代 SHCreateShellFolderViewEx。
        /// 注意：ppsv 用 IntPtr（指向 IShellView* 的指针），不用 out IntPtr，
        /// 避免 .NET marshaling 改变 raw 指针（导致后续 QI 失败）。
        /// </summary>
        [DllImport("shell32.dll")]
        public static extern int SHCreateShellFolderView(SFV_CREATE pcsfv, IntPtr ppsv);

        /// <summary>
        /// 用 Shell 分配器复制字符串（STRRET 用，调用方负责 CoTaskMemFree）。
        /// </summary>
        [DllImport("shlwapi.dll", EntryPoint = "SHStrDupW")]
        public static extern int SHStrDup([MarshalAs(UnmanagedType.LPWStr)] string pszSource, out IntPtr ppwsz);

        /// <summary>
        /// 获取桌面 IShellFolder（根命名空间）。
        /// </summary>
        [DllImport("shell32.dll")]
        public static extern int SHGetDesktopFolder(out IShellFolder ppshf);

        /// <summary>
        /// 释放 Shell 分配的 PIDL。
        /// </summary>
        [DllImport("shell32.dll")]
        public static extern void ILFree(IntPtr pidl);

        /// <summary>
        /// 通知 Shell 注册表/namespace 变化，强制 explorer 刷新缓存。
        /// </summary>
        [DllImport("shell32.dll", CharSet = CharSet.Auto)]
        public static extern void SHChangeNotify(int wEventId, uint uFlags, IntPtr dwItem1, IntPtr dwItem2);
    }
}
