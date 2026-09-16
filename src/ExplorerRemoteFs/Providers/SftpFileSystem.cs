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
                // SSH.NET 只给数字 UserId/GroupId（协议本身就是这样），账户名要自己解析：
                // 读 /etc/passwd 与 /etc/group 建 uid/gid → 名字 的映射（带缓存），
                // 取不到名字就留空，由 OwnerDisplay 退回显示数字。
                Owner = LookupUser(f.UserId),
                Group = LookupGroup(f.GroupId),
                Uid = f.UserId >= 0 ? f.UserId : -1,
                Gid = f.GroupId >= 0 ? f.GroupId : -1,
                Size = f.Length,
                LastWriteTime = f.LastWriteTime,
                SymlinkTarget = f.IsSymbolicLink ? f.FullName : null
            });
        }

        return result;
    }

    // ── uid/gid → 账户名映射 ────────────────────────────────────────────────
    // SFTP 协议只传数字 uid/gid（SSH.NET 的 UserId/GroupId），账户名必须自己解析。
    // 做法：读远端的 /etc/passwd 与 /etc/group（都是几 KB 的小文件），解析成映射表并缓存。
    // 失败一律静默（列里退回显示数字），绝不让"拿不到名字"影响目录列举本身。
    private Dictionary<int, string>? _userNames;
    private Dictionary<int, string>? _groupNames;
    private DateTime _idMapLoadedUtc = DateTime.MinValue;
    private static readonly TimeSpan IdMapTtl = TimeSpan.FromMinutes(10);

    private static string? Lookup(Dictionary<int, string>? map, int id)
    {
        if (map is null || id < 0) return null;
        return map.TryGetValue(id, out var name) ? name : null;
    }

    private string? LookupUser(int uid)
    {
        EnsureIdMaps();
        return Lookup(_userNames, uid);
    }

    private string? LookupGroup(int gid)
    {
        EnsureIdMaps();
        return Lookup(_groupNames, gid);
    }

    private void EnsureIdMaps()
    {
        if (_userNames is not null && DateTime.UtcNow - _idMapLoadedUtc < IdMapTtl) return;
        var users = new Dictionary<int, string>();
        var groups = new Dictionary<int, string>();
        try { ParseIdFile("/etc/passwd", users, nameIndex: 0, idIndex: 3); }
        catch (Exception ex) { Utils.ShellLog.Write($"SFTP /etc/passwd map failed: {ex.Message}"); }
        try { ParseIdFile("/etc/group", groups, nameIndex: 0, idIndex: 2); }
        catch (Exception ex) { Utils.ShellLog.Write($"SFTP /etc/group map failed: {ex.Message}"); }
        _userNames = users;
        _groupNames = groups;
        _idMapLoadedUtc = DateTime.UtcNow;
        Utils.ShellLog.Write($"SFTP id map: users={users.Count} groups={groups.Count}");
    }

    /// <summary>解析 passwd/group 这类 "name:x:id:..." 的冒号分隔文件。</summary>
    private void ParseIdFile(string remotePath, Dictionary<int, string> into, int nameIndex, int idIndex)
    {
        EnsureConnected();
        if (_client is null) return;
        using var ms = new MemoryStream();
        _client.DownloadFile(remotePath, ms);         // 几 KB；不存在会抛，由调用方吞掉
        if (ms.Length == 0 || ms.Length > 4 * 1024 * 1024) return;   // 防御：异常大的文件不解析
        ms.Position = 0;
        using var reader = new StreamReader(ms, System.Text.Encoding.UTF8);
        string? line;
        while ((line = reader.ReadLine()) is not null)
        {
            if (line.Length == 0 || line[0] == '#') continue;
            var parts = line.Split(':');
            if (parts.Length <= Math.Max(nameIndex, idIndex)) continue;
            if (!int.TryParse(parts[idIndex], out int id)) continue;
            var name = parts[nameIndex];
            if (name.Length > 0) into[id] = name;
        }
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

    public void CreateEmptyFile(string path)
    {
        EnsureConnected();
        if (_client is null) return;
        // CreateNew asks the SFTP server to reject an existing path.  This is
        // intentionally not UploadFile: a new-file command must never
        // truncate an existing remote document.
        using var stream = _client.Open(path, FileMode.CreateNew, FileAccess.Write);
        Utils.ShellLog.Write($"SFTP touch: {path}");
    }

    // NOTE: SSH.NET has no resume API; SFTP resume would need OpenWrite + Seek
    // against the remote offset (TODO). The flag is accepted for interface
    // compatibility and currently ignored, so SFTP restarts the transfer.
    public void Download(string remotePath, string localPath, Action<long, long>? progress = null, bool resume = false)
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

    public void Upload(string localPath, string remotePath, Action<long, long>? progress = null, bool resume = false)
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

    public ChmodRecursiveResult SetPermissionsRecursive(string path, int mode,
        Action<string>? onItem = null, CancellationToken cancellation = default)
    {
        var failures = new List<string>();
        int dirs = 0, files = 0;

        // 起点本身也在范围内（chmod -R 的语义包含根目录）。
        cancellation.ThrowIfCancellationRequested();
        onItem?.Invoke(path);
        try { SetPermissions(path, mode); dirs++; }
        catch (Exception ex) { failures.Add($"{path}: {ex.Message}"); }

        ChmodWalk(path, mode, failures, ref dirs, ref files, onItem, cancellation);
        Utils.ShellLog.Write($"SFTP chmod -R: {path} mode={Convert.ToString(mode, 8)} dirs={dirs} files={files} failed={failures.Count}");
        return new ChmodRecursiveResult(dirs, files, failures);
    }

    // 目录与**文件**都要改；符号链接一律跳过（不通过链接去改它目标的权限）。
    // 单项失败只记录、不抛出：树里一个不可写条目不该让整次递归半途而废，
    // 更不该让调用方看到一个"成功"的部分结果。
    // onItem 用于进度显示；cancellation 用于「取消」——取消点放在每个条目之前，
    // 已改过的条目保持已改（不做回滚，与 chmod -R 语义一致）。
    private void ChmodWalk(string path, int mode, List<string> failures, ref int dirs, ref int files,
                           Action<string>? onItem, CancellationToken cancellation)
    {
        cancellation.ThrowIfCancellationRequested();
        IReadOnlyList<RemoteEntry> entries;
        try { entries = List(path); }
        catch (Exception ex) { failures.Add($"{path}: {ex.Message}"); return; }

        foreach (var e in entries)
        {
            cancellation.ThrowIfCancellationRequested();
            if (e.IsSymlink) continue;
            if (e.IsDirectory)
            {
                // 自身改成与否，都必须继续往下走：服务器可能只拒绝目录上的 setstat，
                // 却允许改文件；若在此 return，整棵子树会被静默跳过并从失败清单里消失。
                onItem?.Invoke(e.Path);
                try { SetPermissions(e.Path, mode); dirs++; }
                catch (Exception ex) { failures.Add($"{e.Path}: {ex.Message}"); }
                ChmodWalk(e.Path, mode, failures, ref dirs, ref files, onItem, cancellation);
            }
            else
            {
                onItem?.Invoke(e.Path);
                try { SetPermissions(e.Path, mode); files++; }
                catch (Exception ex) { failures.Add($"{e.Path}: {ex.Message}"); }
            }
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
