namespace ExplorerRemoteFs.Utils;

/// <summary>文件大小的显示口径。与扩展 DLL 的 <c>SizeFormat.h</c> 是**同一套规则**
/// （同一注册表值 <c>HKCU\Software\ExplorerRemoteFs\SizeFormat</c>），两边必须保持一致。
///
/// 为什么要有"口径"这件事：同一串数字有三种都算"正确"的写法 ——
/// Windows 资源管理器用 1024 进制却把标签写成 KB/MB；GNU 的 <c>ls -h</c>/<c>du -h</c>/<c>df -h</c>
/// 默认也是 1024；而 <c>--si</c>/<c>df -H</c>/GNOME 文件、macOS Finder 用 **1000** 进制；
/// 严格按 IEC 80000-13 则是 kB = 1000、KiB = 1024。
/// 远端给的是精确字节数，进制只是显示问题，所以让用户选，**默认取 Windows 口径**
/// （我们长在资源管理器里，与同一窗口的本地文件列一致比"标签更严格"更重要）。
/// </summary>
public enum SizeFormatMode
{
    /// <summary>1024 进制，KB/MB/GB（默认，Windows/资源管理器口径）。</summary>
    Auto,
    /// <summary>1024 进制，整 KB（Windows 属性页那种）。</summary>
    WholeKb,
    /// <summary>1000 进制，kB/MB/GB（与 Nautilus、<c>ls --si</c> 一致）。</summary>
    Si,
    /// <summary>1024 进制，KiB/MiB/GiB（严格 IEC 80000-13）。</summary>
    Iec,
}

public static class SizeFormat
{
    private static readonly string[] SiUnits = { "kB", "MB", "GB", "TB", "PB" };
    private static readonly string[] BinUnits = { "KB", "MB", "GB", "TB", "PB" };
    private static readonly string[] IecUnits = { "KiB", "MiB", "GiB", "TiB", "PiB" };

    /// 当前口径：读 HKCUSoftwarexplorerRemoteFsSizeFormat（与扩展 DLL、客户端设置同一处）。
    /// 缓存 2 秒 —— 列渲染是按行按列调用的，不能每次去读注册表。
    public static SizeFormatMode CurrentMode
    {
        get
        {
            var now = Environment.TickCount64;
            if (_cachedAt != 0 && now - _cachedAt < 2000) return _cached;
            try
            {
                using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(@"SoftwarexplorerRemoteFs");
                _cached = Parse(key?.GetValue("SizeFormat") as string);
            }
            catch { _cached = SizeFormatMode.Auto; }
            _cachedAt = now;
            return _cached;
        }
    }

    private static SizeFormatMode _cached = SizeFormatMode.Auto;
    private static long _cachedAt;

    /// <summary>注册表串 → 口径。非法/空一律回退默认（与 DLL 侧一致）。</summary>
    public static SizeFormatMode Parse(string? value) => (value ?? "").Trim().ToLowerInvariant() switch
    {
        "kb" => SizeFormatMode.WholeKb,
        "si" => SizeFormatMode.Si,
        "iec" => SizeFormatMode.Iec,
        _ => SizeFormatMode.Auto,
    };

    public static string ToSetting(SizeFormatMode mode) => mode switch
    {
        SizeFormatMode.WholeKb => "kb",
        SizeFormatMode.Si => "si",
        SizeFormatMode.Iec => "iec",
        _ => "auto",
    };

    /// <summary>格式化一个字节数。<paramref name="isFolder"/> 为真时返回 "-"（与列里一致）。</summary>
    public static string Format(long bytes, SizeFormatMode mode = SizeFormatMode.Auto,
                                bool isFolder = false)
    {
        if (isFolder) return "-";
        if (mode == SizeFormatMode.WholeKb) return $"{(bytes + 1023) / 1024} KB";

        double @base = mode == SizeFormatMode.Si ? 1000.0 : 1024.0;
        var units = mode == SizeFormatMode.Si ? SiUnits : mode == SizeFormatMode.Iec ? IecUnits : BinUnits;
        if (bytes < @base) return $"{bytes} B";     // 小于一个单位：给字节，别写 0.9 kB

        double shown = bytes / @base;
        int unit = 0;
        while (shown >= @base && unit < units.Length - 1) { shown /= @base; unit++; }
        return $"{shown:0.0} {units[unit]}";   // 恒定一位小数：与 C++ 侧 %.1f 逐字一致（自检抓出过 "1 MB" vs "1.0 MB"）
    }

    /// <summary>口径 + 精确字节（属性页/详情用）："1.2 MB (1,234,567 B)"。</summary>
    public static string FormatWithExact(long bytes, SizeFormatMode mode = SizeFormatMode.Auto,
                                         bool isFolder = false)
    {
        if (isFolder) return "-";
        string human = Format(bytes, mode);
        if (human.EndsWith(" B", StringComparison.Ordinal)) return human;   // 已经是字节数，别重复
        return $"{human} ({bytes:N0} B)";
    }
}

