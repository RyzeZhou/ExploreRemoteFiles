using System;
using Microsoft.Win32;

namespace ExplorerRemoteFs.Utils;

/// <summary>修改时间的显示口径。与扩展 DLL 的 <c>TimeDisplay.h</c> 是**同一套规则**
/// （同一注册表值 <c>HKCU\Software\ExplorerRemoteFs\TimeDisplayMode</c>），两边必须保持一致。
///
/// 为什么要有"口径"这件事：远端给的时间本身就没有唯一答案 ——
/// SFTP 给的是 Unix 秒（绝对时刻，无歧义），而 FTP 的 LIST 只给"服务器本地时间的字面值"，
/// 协议不带时区（RFC 3659 只规定 MDTM 用 UTC）。同一个文件按本地 / 服务器 / UTC 三种口径
/// 显示可以差好几小时，所以让用户选，**默认本地时区**
/// （我们长在资源管理器里，与同一窗口的本地文件列一致比"更接近服务器"更重要）。
/// </summary>
public enum TimeDisplayMode
{
    /// <summary>本地时区（默认）。</summary>
    Local,
    /// <summary>服务器时区；站点未探测到偏移时退回本地。</summary>
    Server,
    /// <summary>UTC+0。</summary>
    Utc,
}

public static class TimeDisplay
{
    private static TimeDisplayMode _cached = TimeDisplayMode.Local;
    private static long _cachedAt;

    /// <summary>当前口径：读 HKCU\Software\ExplorerRemoteFs\TimeDisplayMode（与扩展 DLL、
    /// 客户端设置同一处）。缓存 2 秒 —— 列渲染是按行按列调用的，不能每次去读注册表。</summary>
    public static TimeDisplayMode CurrentMode
    {
        get
        {
            var now = Environment.TickCount64;
            if (_cachedAt != 0 && now - _cachedAt < 2000) return _cached;
            try
            {
                using var key = Registry.CurrentUser.OpenSubKey(@"Software\ExplorerRemoteFs");
                _cached = Parse(key?.GetValue("TimeDisplayMode") as string);
            }
            catch { _cached = TimeDisplayMode.Local; }
            _cachedAt = now;
            return _cached;
        }
    }

    /// <summary>注册表串 → 口径。非法/空一律回退默认（与 DLL 侧一致）。</summary>
    public static TimeDisplayMode Parse(string? value) => (value ?? "").Trim().ToLowerInvariant() switch
    {
        "utc" => TimeDisplayMode.Utc,
        "server" => TimeDisplayMode.Server,
        _ => TimeDisplayMode.Local,
    };

    public static string ToSetting(TimeDisplayMode mode) => mode switch
    {
        TimeDisplayMode.Utc => "utc",
        TimeDisplayMode.Server => "server",
        _ => "local",
    };

    /// <summary>把 **UTC 时刻**按口径渲染成 "yyyy-MM-dd HH:mm"。
    /// <paramref name="serverOffsetMinutes"/> 为 null（站点未探测）时，Server 口径退回本地。</summary>
    public static string Format(DateTime utc, TimeDisplayMode mode, int? serverOffsetMinutes)
    {
        var instant = utc.Kind == DateTimeKind.Utc ? utc : DateTime.SpecifyKind(utc, DateTimeKind.Utc);
        var shown = mode switch
        {
            TimeDisplayMode.Utc => instant,
            TimeDisplayMode.Server when serverOffsetMinutes.HasValue => instant.AddMinutes(serverOffsetMinutes.Value),
            _ => instant.ToLocalTime(),
        };
        return shown.ToString("yyyy-MM-dd HH:mm");
    }

    /// <summary>站点偏移的显示名："UTC+08:00" / "未探测"。</summary>
    public static string DescribeOffset(int? minutes)
    {
        if (!minutes.HasValue) return "未探测";
        var span = TimeSpan.FromMinutes(minutes.Value);
        return $"UTC{(span < TimeSpan.Zero ? "-" : "+")}{span.Duration():hh\\:mm}";
    }
}
