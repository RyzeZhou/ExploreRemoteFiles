namespace ExplorerRemoteFs.Config;

/// <summary>
/// 一个远程连接配置（对应探索计划 §4 P5：Location 模型）。
/// </summary>
public sealed class ConnectionConfig
{
    /// <summary>显示名（出现在 Explorer 导航中）。</summary>
    public string Name { get; set; } = "New Server";

    /// <summary>协议类型：sftp / ftp / ftps。</summary>
    public string Type { get; set; } = "sftp";

    /// <summary>主机名或 IP。</summary>
    public string Host { get; set; } = "localhost";

    /// <summary>端口（null 时按协议默认：SFTP=22，FTP=21）。</summary>
    public int? Port { get; set; }

    /// <summary>用户名。</summary>
    public string Username { get; set; } = "";

    /// <summary>密码（仅本机 JSON 存储）。</summary>
    public string? Password { get; set; }

    /// <summary>可选私钥路径（SFTP）。</summary>
    public string? PrivateKeyPath { get; set; }

    /// <summary>浏览起点（默认 "/"）。</summary>
    public string StartPath { get; set; } = "/";

    public int EffectivePort => Port ?? (Type.StartsWith("sftp", StringComparison.OrdinalIgnoreCase) ? 22 : 21);
}
