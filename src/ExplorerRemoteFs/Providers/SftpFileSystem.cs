using ExplorerRemoteFs.Config;
using Renci.SshNet;
using Renci.SshNet.Common;
using Renci.SshNet.Sftp;
using System.IO;

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
                // SSH.NET exposes numeric UserId/GroupId but no account names;
                // name columns stay empty (numeric columns carry the values).
                Owner = null,
                Group = null,
                Uid = f.UserId >= 0 ? f.UserId : -1,
                Gid = f.GroupId >= 0 ? f.GroupId : -1,
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

    public void Download(string remotePath, string localPath, Action<long, long>? progress = null)
    {
        EnsureConnected();
        if (_client is null) return;
        long total = 0;
        try { total = _client.GetAttributes(remotePath).Size; } catch { }
        using var fs = File.Create(localPath);
        _client.DownloadFile(remotePath, fs, downloaded => progress?.Invoke((long)downloaded, total));
        progress?.Invoke(new FileInfo(localPath).Length, new FileInfo(localPath).Length);
        Utils.ShellLog.Write($"SFTP get: {remotePath} -> {localPath}");
    }

    public void Upload(string localPath, string remotePath, Action<long, long>? progress = null)
    {
        EnsureConnected();
        if (_client is null) return;
        long total = 0;
        try { total = new FileInfo(localPath).Length; } catch { }
        using var fs = File.OpenRead(localPath);
        _client.UploadFile(fs, remotePath, uploaded => progress?.Invoke((long)uploaded, total));
        progress?.Invoke(total, total);
        Utils.ShellLog.Write($"SFTP put: {localPath} -> {remotePath}");
    }

    public void SetPermissions(string path, int mode)
    {
        EnsureConnected();
        if (_client is null) return;
        // mode 是八进制数值（如 0640 的数值 416）。SSH.NET 的 ChangePermissions 把
        // 传入值按十进制 ToString 发给服务器（服务器按八进制解释），
        // 所以这里必须还原为"八进制字符串的十进制形式"（640 → 传 640）。
        short wire = (short)int.Parse(Convert.ToString(mode, 8));
        _client.ChangePermissions(path, wire);
        Utils.ShellLog.Write($"SFTP chmod: {path} = {Convert.ToString(mode, 8)}");
    }

    public void SetOwner(string path, string? user, string? group)
    {
        EnsureConnected();
        if (_client is null) return;
        // SFTP 协议（SSH_FXP_SETSTAT）可写 uid/gid；SSH.NET 通过
        // GetAttributes → 改 UserId/GroupId → SetAttributes 实现（SetLastWriteTime 同模式）。
        // 名字解析（user → uid）需要服务器侧命令，暂仅支持数字。
        var attrs = _client.GetAttributes(path)
            ?? throw new InvalidOperationException($"No such file: {path}");
        bool changed = false;
        if (!string.IsNullOrEmpty(user))
        {
            if (!uint.TryParse(user, out uint uid))
                throw new InvalidOperationException($"UID must be numeric: {user}");
            attrs.UserId = (int)uid;
            changed = true;
        }
        if (!string.IsNullOrEmpty(group))
        {
            if (!uint.TryParse(group, out uint gid))
                throw new InvalidOperationException($"GID must be numeric: {group}");
            attrs.GroupId = (int)gid;
            changed = true;
        }
        if (changed) _client.SetAttributes(path, attrs);
        Utils.ShellLog.Write($"SFTP chown: {path} user={user} group={group}");
    }

    public void SetPermissionsRecursive(string path, int mode)
    {
        SetPermissions(path, mode);
        foreach (var e in List(path))
        {
            if (e.IsDirectory && !e.IsSymlink)
                SetPermissionsRecursive(e.Path, mode);
        }
    }

    public void Copy(string from, string to)
    {
        EnsureConnected();
        if (_client is null) return;
        using var ms = new MemoryStream();
        _client.DownloadFile(from, ms);
        ms.Position = 0;
        _client.UploadFile(ms, to);
        Utils.ShellLog.Write($"SFTP dup: {from} -> {to}");
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
