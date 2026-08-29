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

    public int EffectivePort => Port ?? (Type.StartsWith("sftp", StringComparison.OrdinalIgnoreCase) ? 22 : 21);
}
