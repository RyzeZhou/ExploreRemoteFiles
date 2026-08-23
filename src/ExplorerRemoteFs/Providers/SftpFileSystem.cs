using ExplorerRemoteFs.Config;
using Renci.SshNet;
using Renci.SshNet.Common;
using Renci.SshNet.Sftp;

namespace ExplorerRemoteFs.Providers;

/// <summary>
/// SFTP Provider（基于 SSH.NET，MIT）。提供 POSIX 语义：mode / uid / gid / symlink。
/// </summary>
public sealed class SftpFileSystem : IRemoteFileSystem
{
    private readonly ConnectionConfig _config;
    private SftpClient? _client;

    public SftpFileSystem(ConnectionConfig config)
    {
        _config = config;
    }

    public string DisplayName => $"SFTP {_config.Username}@{_config.Host}:{_config.EffectivePort}";

    public void EnsureConnected()
    {
        if (_client is { IsConnected: true }) return;

        _client?.Dispose();

        _client = CreateClient();
        _client.ConnectionInfo.Timeout = TimeSpan.FromSeconds(10);
        _client.OperationTimeout = TimeSpan.FromSeconds(15);
        _client.Connect();
        Utils.ShellLog.Write($"SFTP connected: {DisplayName}");
    }

    private SftpClient CreateClient()
    {
        if (!string.IsNullOrEmpty(_config.PrivateKeyPath))
        {
            var keyFile = new PrivateKeyFile(_config.PrivateKeyPath);
            return new SftpClient(_config.Host, _config.EffectivePort, _config.Username, keyFile);
        }

        return new SftpClient(_config.Host, _config.EffectivePort, _config.Username, _config.Password ?? "");
    }

    public IReadOnlyList<RemoteEntry> List(string path)
    {
        EnsureConnected();
        if (_client is null) return Array.Empty<RemoteEntry>();

        var result = new List<RemoteEntry>();
        IEnumerable<ISftpFile> files;
        try
        {
            files = _client.ListDirectory(path);
        }
        catch (SftpPathNotFoundException)
        {
            return result;
        }

        foreach (var f in files)
        {
            if (f.Name is "." or "..") continue;

            var isDir = f.IsDirectory;
            result.Add(new RemoteEntry
            {
                Name = f.Name,
                Path = f.FullName,
                IsDirectory = isDir,
                IsSymlink = f.IsSymbolicLink,
                Mode = FormatMode(f, isDir, f.IsSymbolicLink),
                Owner = f.UserId >= 0 ? f.UserId.ToString() : null,
                Group = f.GroupId >= 0 ? f.GroupId.ToString() : null,
                Size = f.Length,
                LastWriteTime = f.LastWriteTime,
                SymlinkTarget = f.IsSymbolicLink ? f.FullName : null
            });
        }

        return result;
    }

    /// <summary>ISftpFile 权限布尔属性 → "drwxr-xr-x" 风格字符串。</summary>
    private static string FormatMode(ISftpFile f, bool isDir, bool isSymlink)
    {
        var sb = new System.Text.StringBuilder(10);
        sb.Append(isSymlink ? 'l' : isDir ? 'd' : '-');
        sb.Append(f.OwnerCanRead ? 'r' : '-');
        sb.Append(f.OwnerCanWrite ? 'w' : '-');
        sb.Append(f.OwnerCanExecute ? 'x' : '-');
        sb.Append(f.GroupCanRead ? 'r' : '-');
        sb.Append(f.GroupCanWrite ? 'w' : '-');
        sb.Append(f.GroupCanExecute ? 'x' : '-');
        sb.Append(f.OthersCanRead ? 'r' : '-');
        sb.Append(f.OthersCanWrite ? 'w' : '-');
        sb.Append(f.OthersCanExecute ? 'x' : '-');
        return sb.ToString();
    }

    public void Delete(string path)
    {
        EnsureConnected();
        if (_client is null) return;
        var item = _client.GetAttributes(path);
        if (item is null) return;
        if (item.IsDirectory)
            _client.DeleteDirectory(path);
        else
            _client.DeleteFile(path);
        Utils.ShellLog.Write($"SFTP deleted: {path}");
    }

    public void Rename(string from, string to)
    {
        EnsureConnected();
        if (_client is null) return;
        _client.RenameFile(from, to);
        Utils.ShellLog.Write($"SFTP renamed: {from} -> {to}");
    }

    public void CreateDirectory(string path)
    {
        EnsureConnected();
        if (_client is null) return;
        _client.CreateDirectory(path);
        Utils.ShellLog.Write($"SFTP mkdir: {path}");
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
