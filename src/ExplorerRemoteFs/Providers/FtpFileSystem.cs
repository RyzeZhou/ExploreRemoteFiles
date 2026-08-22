using ExplorerRemoteFs.Config;
using FluentFTP;

namespace ExplorerRemoteFs.Providers;

/// <summary>
/// FTP / FTPS Provider（基于 FluentFTP，MIT）。
/// capability-based 降级：FTP server 元数据支持差异大，mode/owner/group 仅在 Unix 风格服务器提供时显示。
/// </summary>
public sealed class FtpFileSystem : IRemoteFileSystem
{
    private readonly ConnectionConfig _config;
    private FtpClient? _client;

    public FtpFileSystem(ConnectionConfig config)
    {
        _config = config;
    }

    public string DisplayName => $"{_config.Type.ToUpperInvariant()} {_config.Username}@{_config.Host}:{_config.EffectivePort}";

    public void EnsureConnected()
    {
        if (_client is { IsConnected: true }) return;

        _client?.Dispose();

        _client = new FtpClient(_config.Host, _config.Username, _config.Password ?? "", _config.EffectivePort)
        {
            Config =
            {
                ConnectTimeout = 10_000,
                ReadTimeout = 15_000,
                DataConnectionConnectTimeout = 10_000
            }
        };
        if (_config.Type.Equals("ftps", StringComparison.OrdinalIgnoreCase))
        {
            _client.Config.EncryptionMode = FtpEncryptionMode.Explicit;
            _client.Config.ValidateAnyCertificate = false;
        }
        _client.Connect();
        Utils.ShellLog.Write($"FTP connected: {DisplayName}");
    }

    public IReadOnlyList<RemoteEntry> List(string path)
    {
        EnsureConnected();
        if (_client is null) return Array.Empty<RemoteEntry>();

        var result = new List<RemoteEntry>();
        FtpListItem[] items;
        try
        {
            items = _client.GetListing(path);
        }
        catch (System.IO.IOException ex)
        {
            Utils.ShellLog.Write($"FTP List failed {path}: {ex.Message}");
            return result;
        }

        foreach (var item in items)
        {
            if (item.Name is "." or "..") continue;

            var isDir = item.Type == FtpObjectType.Directory;
            result.Add(new RemoteEntry
            {
                Name = item.Name,
                Path = item.FullName,
                IsDirectory = isDir,
                IsSymlink = item.Type == FtpObjectType.Link,
                Mode = FormatMode(item.OwnerPermissions, item.GroupPermissions, item.OthersPermissions, isDir),
                Owner = string.IsNullOrEmpty(item.RawOwner) ? null : item.RawOwner,
                Group = string.IsNullOrEmpty(item.RawGroup) ? null : item.RawGroup,
                Size = item.Size,
                LastWriteTime = item.Modified == DateTime.MinValue ? (DateTime?)null : item.Modified,
                SymlinkTarget = string.IsNullOrEmpty(item.LinkTarget) ? null : item.LinkTarget
            });
        }

        return result;
    }

    /// <summary>Owner/Group/Others 三组 FtpPermission → "drwxr-xr-x"（Unix 服务器解析才有值，否则返回 null）。</summary>
    private static string? FormatMode(FtpPermission owner, FtpPermission group, FtpPermission others, bool isDir)
    {
        if (owner == FtpPermission.None && group == FtpPermission.None && others == FtpPermission.None) return null;
        var sb = new System.Text.StringBuilder(10);
        sb.Append(isDir ? 'd' : '-');
        sb.Append(owner.HasFlag(FtpPermission.Read) ? 'r' : '-');
        sb.Append(owner.HasFlag(FtpPermission.Write) ? 'w' : '-');
        sb.Append(owner.HasFlag(FtpPermission.Execute) ? 'x' : '-');
        sb.Append(group.HasFlag(FtpPermission.Read) ? 'r' : '-');
        sb.Append(group.HasFlag(FtpPermission.Write) ? 'w' : '-');
        sb.Append(group.HasFlag(FtpPermission.Execute) ? 'x' : '-');
        sb.Append(others.HasFlag(FtpPermission.Read) ? 'r' : '-');
        sb.Append(others.HasFlag(FtpPermission.Write) ? 'w' : '-');
        sb.Append(others.HasFlag(FtpPermission.Execute) ? 'x' : '-');
        return sb.ToString();
    }

    public void Dispose()
    {
        try
        {
            _client?.Disconnect();
        }
        catch
        {
            // ignore
        }
        _client?.Dispose();
        _client = null;
    }
}
