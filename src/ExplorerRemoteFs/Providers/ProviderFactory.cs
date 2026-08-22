using System.Collections.Concurrent;
using ExplorerRemoteFs.Config;

namespace ExplorerRemoteFs.Providers;

/// <summary>
/// Provider 工厂 + 连接池：同一连接名复用 session（探索计划 §10：persistent session / connection reuse）。
/// 空闲超过 10 分钟的连接自动重建；连接失败时从池中清除。
/// </summary>
public static class ProviderFactory
{
    private sealed class PoolEntry
    {
        public required IRemoteFileSystem Fs { get; init; }
        public DateTime LastUsed { get; set; } = DateTime.UtcNow;
    }

    private static readonly TimeSpan IdleTimeout = TimeSpan.FromMinutes(10);

    private static readonly ConcurrentDictionary<string, PoolEntry> Pool = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>获取（或创建并连接）一个 Provider。</summary>
    public static IRemoteFileSystem Get(ConnectionConfig config)
    {
        var key = config.Name;

        if (Pool.TryGetValue(key, out var entry))
        {
            if (DateTime.UtcNow - entry.LastUsed <= IdleTimeout)
            {
                entry.LastUsed = DateTime.UtcNow;
                try
                {
                    entry.Fs.EnsureConnected();
                    return entry.Fs;
                }
                catch (Exception ex)
                {
                    Utils.ShellLog.Write($"Pool reconnect failed for '{key}': {ex.Message}");
                    Invalidate(key);
                }
            }
            else
            {
                Invalidate(key);
            }
        }

        // 创建并连接；失败立即抛出（调用方转为错误项展示，不让 Explorer 卡死）。
        var fs = Create(config);
        fs.EnsureConnected();
        Pool[key] = new PoolEntry { Fs = fs };
        return fs;
    }

    public static IRemoteFileSystem Create(ConnectionConfig config)
    {
        return config.Type.StartsWith("sftp", StringComparison.OrdinalIgnoreCase)
            ? new SftpFileSystem(config)
            : new FtpFileSystem(config);
    }

    /// <summary>移除并释放池中连接（连接失败 / 用户修改配置后调用）。</summary>
    public static void Invalidate(string name)
    {
        if (Pool.TryRemove(name, out var entry))
        {
            try { entry.Fs.Dispose(); } catch { /* ignore */ }
        }
    }
}
