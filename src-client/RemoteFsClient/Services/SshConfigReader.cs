using System.IO;

namespace RemoteFsClient.Services;

/// <summary>%USERPROFILE%\.ssh\config 里的一个 Host 块（解析出的有效值）。</summary>
public sealed record SshHostEntry(
    string Alias,
    IReadOnlyList<string> Names,
    string HostName,
    string? User,
    int? Port,
    string? IdentityFile)
{
    /// <summary>下拉里显示的摘要：别名 → user@host:port</summary>
    public string Summary
    {
        get
        {
            var target = string.IsNullOrEmpty(User) ? HostName : $"{User}@{HostName}";
            return Port is int p ? $"{Alias}  →  {target}:{p}" : $"{Alias}  →  {target}";
        }
    }
}

/// <summary>
/// 读取并解析用户的 SSH 配置（%USERPROFILE%\.ssh\config）。
///
/// 为什么需要它：「在此打开终端 / VS Code」应当**复用用户已有的 SSH 连接**，
/// 而不是另建一套。绑定前必须校验主机、端口、用户名一致 —— 否则会出现
/// 「在本站点上右键，却登进了另一台机器或另一个账号」这种严重错误。
///
/// 解析范围（有意保守）：
///   * 只处理 Host 块；支持 `Key value` 与 `Key=value` 两种写法，大小写不敏感；
///   * 含通配符（* ? !）的 Host 名不作为可绑定别名（它们是模式，不是具体主机）；
///   * `Match` 块与 `Include` 指令不展开（安全优先：宁可不列，也不猜）；
///   * 同一 Host 行有多个别名时，第一个作为可绑定别名。
/// </summary>
public static class SshConfigReader
{
    public static string ConfigPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".ssh", "config");

    public static IReadOnlyList<SshHostEntry> Read()
    {
        var entries = new List<SshHostEntry>();
        try
        {
            if (!File.Exists(ConfigPath)) return entries;

            List<string> names = new();
            string? hostName = null, user = null, identity = null;
            int? port = null;
            bool inMatch = false;

            void Flush()
            {
                if (names.Count > 0)
                {
                    // 第一个非通配符别名作为绑定名
                    var alias = names.FirstOrDefault(n => !IsPattern(n));
                    if (alias != null)
                    {
                        // 与 ssh 语义一致：没写 HostName 时，连接用的主机名就是命令行上的别名本身
                        // （`Host 192.168.200.1` + `Port/User` 这种写法很常见，必须支持绑定）
                        var effectiveHost = string.IsNullOrWhiteSpace(hostName) ? alias : hostName!;
                        entries.Add(new SshHostEntry(alias, names.ToList(), effectiveHost, user, port, identity));
                    }
                }
                names = new List<string>();
                hostName = null; user = null; identity = null; port = null;
            }

            foreach (var raw in File.ReadLines(ConfigPath))
            {
                var line = raw.Trim();
                if (line.Length == 0 || line.StartsWith('#')) continue;

                // 支持 Key value / Key=value
                int sep = line.IndexOfAny(new[] { ' ', '\t', '=' });
                string key, value;
                if (sep < 0) { key = line; value = ""; }
                else { key = line[..sep]; value = line[(sep + 1)..].Trim(); }

                if (key.Equals("Host", StringComparison.OrdinalIgnoreCase))
                {
                    Flush();
                    inMatch = false;
                    names = value.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries).ToList();
                }
                else if (key.Equals("Match", StringComparison.OrdinalIgnoreCase))
                {
                    // Match 块的有效值依赖运行时条件，无法静态判定 → 结束当前块并忽略
                    Flush();
                    inMatch = true;
                }
                else if (key.Equals("Include", StringComparison.OrdinalIgnoreCase))
                {
                    // 不展开 Include（安全优先）
                }
                else if (!inMatch && names.Count > 0)
                {
                    if (key.Equals("HostName", StringComparison.OrdinalIgnoreCase)) hostName = value;
                    else if (key.Equals("User", StringComparison.OrdinalIgnoreCase)) user = value;
                    else if (key.Equals("IdentityFile", StringComparison.OrdinalIgnoreCase)) identity = value;
                    else if (key.Equals("Port", StringComparison.OrdinalIgnoreCase) && int.TryParse(value, out var p)) port = p;
                }
            }
            Flush();
        }
        catch (Exception ex)
        {
            Log($"SshConfigReader.Read failed: {ex.Message}");
        }
        return entries;
    }

    /// <summary>诊断日志（%TEMP%/rfs-tasks.log）——与 TransferTaskService 同一份，便于排错。</summary>
    private static void Log(string message)
    {
        try
        {
            File.AppendAllText(Path.Combine(Path.GetTempPath(), "rfs-tasks.log"),
                DateTime.Now.ToString("HH:mm:ss.fff") + " " + message + Environment.NewLine);
        }
        catch { }
    }

    private static bool IsPattern(string name) =>
        name.Contains('*') || name.Contains('?') || name.StartsWith('!');

    /// <summary>
    /// 校验一条 SSH 配置是否与站点「同一台机器、同一个用户」：
    /// 主机名必须相同（忽略大小写），用户名必须相同，端口必须相同
    /// （配置未写 Port 时按 SSH 默认 22 计算）。任一项不符即拒绝绑定。
    /// </summary>
    public static bool MatchesSite(SshHostEntry entry, string? host, int sitePort, string? user)
    {
        if (!string.Equals(entry.HostName.Trim(), (host ?? "").Trim(), StringComparison.OrdinalIgnoreCase))
            return false;
        if (!string.Equals((entry.User ?? "").Trim(), (user ?? "").Trim(), StringComparison.OrdinalIgnoreCase))
            return false;
        int entryPort = entry.Port ?? 22;
        return entryPort == sitePort;
    }

    /// <summary>与站点主机相同、但用户名或端口不同 —— 用于提示「为什么不列出」。</summary>
    public static IReadOnlyList<SshHostEntry> SameHostButDifferentIdentity(
        IEnumerable<SshHostEntry> all, string? host, int sitePort, string? user) =>
        all.Where(e => string.Equals(e.HostName.Trim(), (host ?? "").Trim(), StringComparison.OrdinalIgnoreCase)
                       && !MatchesSite(e, host, sitePort, user)).ToList();
}
