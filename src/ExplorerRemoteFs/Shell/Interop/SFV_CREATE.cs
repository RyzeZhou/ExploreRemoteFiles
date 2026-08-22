using System;
using System.Runtime.InteropServices;

namespace ExplorerRemoteFs.Shell.Interop
{
    /// <summary>
    /// SFV_CREATE — 与 SHCreateShellFolderView 配套使用（规范第 6 节）。
    /// psvOuter/psfvcb 必须是 IntPtr（可空），不能是接口引用，
    /// 否则 .NET COM 调度对 null 引用 QI 时崩溃 → explorer 重试死循环。
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct SFV_CREATE
    {
        public uint cbSize;
        public IntPtr pshf;          // IShellFolder* — 用 Marshal.GetIUnknownForObject(this) 装箱
        public IntPtr psvOuter;      // IShellView* 外壳（NULL = 0）
        public IntPtr psfvc;         // IShellFolderViewCB* 或函数指针（NULL = 0）
    }
}
