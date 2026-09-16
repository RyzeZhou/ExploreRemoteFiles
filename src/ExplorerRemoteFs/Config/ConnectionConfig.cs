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

    /// <summary>FTP/FTPS 文件名编码：默认强制 UTF-8；关闭后由服务器自动协商。</summary>
    public bool FtpUseUtf8 { get; set; } = true;

    /// <summary>
    /// 该站点的默认终端程序（「在此打开终端」用）：null/空 = 跟随全局设置；
    /// 取值 wt / powershell / vscode。仅对 SFTP（SSH）站点有意义。
    /// </summary>
    public string? Terminal { get; set; }

    /// <summary>
    /// 绑定的现有 SSH 配置 Host 别名（%USERPROFILE%\.ssh\config 里的 Host）。
    /// 绑定时已校验主机/端口/用户名与站点一致，避免登错机器或用户。
    /// null/空 = 未绑定（调用方按主机+端口+用户名自动匹配，或用受管配置）。
    /// </summary>
    public string? SshHostAlias { get; set; }

    /// <summary>
    /// 列顺序：9 个数字（0..8 的排列），第 i 位 = 显示列号 i 对应的语义属性号
    /// （0=名称 1=类型 2=大小 3=修改时间 4=权限 5=所有者 6=所有者ID 7=组 8=组ID）。
    /// null/空 = 默认 "012345678"。
    /// 注意：本类还被 CLI 的 add/编辑流程整体 Load→Save 回写（见 CmdAdd），
    /// 新增站点字段必须同时加到这里，否则会被静默丢掉。
    /// </summary>
    public string? ColumnOrder { get; set; }

    public int EffectivePort => Port ?? (Type.StartsWith("sftp", StringComparison.OrdinalIgnoreCase) ? 22 : 21);
}
