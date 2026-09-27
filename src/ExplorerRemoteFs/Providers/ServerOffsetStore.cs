using ExplorerRemoteFs.Config;

namespace ExplorerRemoteFs.Providers;

/// <summary>
/// 探测到的服务器时区偏移的落盘处。
///
/// 为什么需要它：SFTP 协议只给 Unix 秒（绝对时刻，不带时区），FTP 的 LIST 只给
/// "服务器本地时间"的字面值（RFC 3659 只规定 MDTM 用 UTC）——**两个协议都不传时区**。
/// 所以「按服务器时区显示」这个口径只能靠探测，而探测结果必须记住，
/// 否则每次新进程都要重探一遍（服务与 CLI 是不同进程，各探各的）。
/// </summary>
internal static class ServerOffsetStore
{
    /// <summary>把探测到的偏移写回站点配置：内存里的那个实例 + connections.json。
    /// 写盘失败**不影响本次显示**（只是下次还要再探一次），所以这里吞掉异常。</summary>
    public static void Persist(ConnectionConfig config, int minutes)
    {
        config.ServerUtcOffsetMinutes = minutes;
        try
        {
            var all = ConnectionStore.Load();
            var target = all.FirstOrDefault(c => c.Name.Equals(config.Name, StringComparison.OrdinalIgnoreCase));
            if (target is null || target.ServerUtcOffsetMinutes == minutes) return;
            target.ServerUtcOffsetMinutes = minutes;
            ConnectionStore.Save(all);
        }
        catch { /* 写盘失败不影响本次显示 */ }
    }
}
