using System.Runtime.InteropServices;
using System.Text;

namespace ExplorerRemoteFs.Shell;

/// <summary>
/// PIDL 辅助：把 item 标识字符串编码为 SHITEMID 链（abData = UTF-8 字符串）。
/// 布局：每段 = [ushort cb][data]，链以 ushort 0 终结。
/// 内存用 CoTaskMem 分配（由 Shell 释放）。
/// </summary>
public static class PidlHelper
{
    private const int MaxSegments = 32;      // 防遍历死循环
    private const int MaxSegmentLen = 4096;  // 单段数据上限

    /// <summary>空 PIDL（仅链终结符，表示根节点自身）。</summary>
    public static IntPtr Empty()
    {
        var pidl = Marshal.AllocCoTaskMem(2);
        Marshal.WriteInt16(pidl, 0);
        return pidl;
    }

    /// <summary>创建一个单段 PIDL：[cb][data][cb=0 终结]。</summary>
    public static IntPtr Create(string id)
    {
        var data = Encoding.UTF8.GetBytes(id);
        var segLen = 2 + data.Length;          // 段长度（不含链终结符）
        var total = segLen + 2;                // 段 + 链终结符
        var pidl = Marshal.AllocCoTaskMem(total);
        Marshal.WriteInt16(pidl, 0, (short)segLen);
        if (data.Length > 0)
            Marshal.Copy(data, 0, IntPtr.Add(pidl, 2), data.Length);
        Marshal.WriteInt16(IntPtr.Add(pidl, segLen), 0); // 链终结符
        return pidl;
    }

    /// <summary>在父 PIDL 后追加一段，形成子项 PIDL。</summary>
    public static IntPtr Append(IntPtr parent, string id)
    {
        var data = Encoding.UTF8.GetBytes(id);
        var parentBytes = new byte[GetPidlSize(parent) - 2]; // 父（不含链终结符）
        if (parentBytes.Length > 0)
            Marshal.Copy(parent, parentBytes, 0, parentBytes.Length);

        var segLen = 2 + data.Length;
        var total = parentBytes.Length + segLen + 2;
        var pidl = Marshal.AllocCoTaskMem(total);
        if (parentBytes.Length > 0)
            Marshal.Copy(parentBytes, 0, pidl, parentBytes.Length);
        var segPos = parentBytes.Length;
        Marshal.WriteInt16(pidl, segPos, (short)segLen);
        if (data.Length > 0)
            Marshal.Copy(data, 0, IntPtr.Add(pidl, segPos + 2), data.Length);
        Marshal.WriteInt16(IntPtr.Add(pidl, segPos + segLen), 0); // 链终结符
        return pidl;
    }

    /// <summary>读取 PIDL 总字节数（含链终结符）。带段数与长度保护，防止非法 PIDL 死循环。</summary>
    public static int GetPidlSize(IntPtr pidl)
    {
        if (pidl == IntPtr.Zero) return 2;
        var size = 0;
        var segments = 0;
        while (segments++ < MaxSegments)
        {
            var cb = Marshal.ReadInt16(pidl, size);
            if (cb == 0) return size + 2;
            if (cb < 2 || cb > MaxSegmentLen) return size + 2; // 非法段，截断返回
            size += cb;
        }
        return size + 2;
    }

    /// <summary>取 PIDL 最后一段的字符串标识。</summary>
    public static string? GetLastId(IntPtr pidl)
    {
        if (pidl == IntPtr.Zero) return null;
        var pos = 0;
        string? last = null;
        var segments = 0;
        while (segments++ < MaxSegments)
        {
            var cb = Marshal.ReadInt16(pidl, pos);
            if (cb == 0) return last;
            if (cb < 2 || cb > MaxSegmentLen) return last; // 非法段，返回已有结果
            var len = cb - 2;
            if (len > 0)
            {
                var bytes = new byte[len];
                Marshal.Copy(IntPtr.Add(pidl, pos + 2), bytes, 0, len);
                last = Encoding.UTF8.GetString(bytes).TrimEnd('\0');
            }
            pos += cb;
        }
        return last;
    }

    /// <summary>克隆 PIDL（CoTaskMem 新分配）。</summary>
    public static IntPtr Clone(IntPtr pidl)
    {
        if (pidl == IntPtr.Zero) return Empty();
        var size = GetPidlSize(pidl);
        var copy = Marshal.AllocCoTaskMem(size);
        var bytes = new byte[size];
        Marshal.Copy(pidl, bytes, 0, size);
        Marshal.Copy(bytes, 0, copy, size);
        return copy;
    }

    /// <summary>释放 PIDL。</summary>
    public static void Free(IntPtr pidl)
    {
        if (pidl != IntPtr.Zero) Marshal.FreeCoTaskMem(pidl);
    }
}
