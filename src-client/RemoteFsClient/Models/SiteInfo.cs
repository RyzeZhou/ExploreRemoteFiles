namespace RemoteFsClient.Models;

/// <summary>
/// 站点配置——与 ExplorerRemoteFs.Config.ConnectionConfig 字段兼容，
/// 共享同一份 %APPDATA%\ExplorerRemoteFs\connections.json。
/// 密码不存 JSON，统一放 Windows 凭据管理器（DPAPI）。
/// </summary>
public sealed class SiteInfo
{
    public string Name { get; set; } = "New Server";
    public string Type { get; set; } = "sftp";          // sftp / ftp / ftps
    public string Host { get; set; } = "localhost";
    public int? Port { get; set; }
    public string Username { get; set; } = "";
    public string? Password { get; set; }               // 兼容旧字段；恒为 null
    public string? PrivateKeyPath { get; set; }
    public string StartPath { get; set; } = "/";
    public bool FtpUseUtf8 { get; set; } = true;

    /// <summary>站点级默认终端程序：null/空 = 跟随全局；wt / powershell / vscode。</summary>
    public string? Terminal { get; set; }

    /// <summary>绑定的现有 SSH Host 别名（%USERPROFILE%\.ssh\config）；null = 未绑定。</summary>
    public string? SshHostAlias { get; set; }

    /// <summary>本站点是否具备 SSH 通道（只有 SFTP/SCP 才有 shell 可用）。</summary>
    [System.Text.Json.Serialization.JsonIgnore]
    public bool IsSshCapable => Type.StartsWith("sftp", StringComparison.OrdinalIgnoreCase)
                             || Type.StartsWith("scp", StringComparison.OrdinalIgnoreCase);

    [System.Text.Json.Serialization.JsonIgnore]
    public int EffectivePort => Port ?? (Type.StartsWith("sftp", StringComparison.OrdinalIgnoreCase) ? 22 : 21);

    public override string ToString() => Name;
}
