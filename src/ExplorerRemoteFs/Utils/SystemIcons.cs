using System.Runtime.InteropServices;

namespace ExplorerRemoteFs.Utils;

/// <summary>
/// 通过 SHGetFileInfo 获取 Windows 系统图标（文件夹 / 按扩展名文件），避免自绘图标。
/// </summary>
public static class SystemIcons
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
    private struct SHFILEINFO
    {
        public IntPtr hIcon;
        public int iIcon;
        public uint dwAttributes;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
        public string szDisplayName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 80)]
        public string szTypeName;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Auto)]
    private static extern IntPtr SHGetFileInfo(string pszPath, uint dwFileAttributes, out SHFILEINFO psfi,
        uint cbFileInfo, uint uFlags);

    private const uint SHGFI_ICON = 0x100;
    private const uint SHGFI_USEFILEATTRIBUTES = 0x10;
    private const uint FILE_ATTRIBUTE_DIRECTORY = 0x10;

    private static Icon? _folderIcon;
    private static readonly Dictionary<string, Icon?> FileIconCache = new(StringComparer.OrdinalIgnoreCase);

    public static Icon Folder()
    {
        return _folderIcon ??= Extract("", FILE_ATTRIBUTE_DIRECTORY) ?? System.Drawing.SystemIcons.Application;
    }

    public static Icon File(string fileName)
    {
        var key = Path.GetExtension(fileName);
        if (string.IsNullOrEmpty(key)) key = ".file";
        if (FileIconCache.TryGetValue(key, out var icon)) return icon ?? System.Drawing.SystemIcons.Application;
        icon = Extract(fileName, 0) ?? System.Drawing.SystemIcons.Application;
        FileIconCache[key] = icon;
        return icon;
    }

    private static Icon? Extract(string path, uint attributes)
    {
        var info = new SHFILEINFO();
        var flags = SHGFI_ICON | SHGFI_USEFILEATTRIBUTES;
        var ret = SHGetFileInfo(path, attributes, out info, (uint)Marshal.SizeOf<SHFILEINFO>(), flags);
        if (ret == IntPtr.Zero || info.hIcon == IntPtr.Zero) return null;
        try
        {
            return (Icon)Icon.FromHandle(info.hIcon).Clone();
        }
        finally
        {
            // 由调用方持有克隆，销毁原始句柄
            if (info.hIcon != IntPtr.Zero) DestroyIcon(info.hIcon);
        }
    }

    [DllImport("user32.dll")]
    private static extern bool DestroyIcon(IntPtr hIcon);
}
