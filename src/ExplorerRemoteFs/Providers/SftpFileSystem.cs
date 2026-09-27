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

        EnsureServerOffset();

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
                LastWriteTime = ToUtc(f.LastWriteTime),
                ServerUtcOffsetMinutes = ServerOffsetMinutes,
                SymlinkTarget = f.IsSymbolicLink ? f.FullName : null
            });
        }

        return result;
    }

    /// <summary>
    /// SSH.NET 的 <c>SftpFileAttributes.LastWriteTime</c> 实测是**本地时间的字面值**
    /// （Kind=Local/Unspecified，值已经由 SSH.NET 从协议里的 Unix 秒本地化过），
    /// 这里统一成 UTC 时刻：否则下游 <c>ToUniversalTime()</c> 会按 Kind 各自解释，
    /// 同一条链路（CLI 直出 vs 桥接转 epoch 再由 DLL 本地化）会得出两种结果。
    /// </summary>
    private static DateTime ToUtc(DateTime value) =>
        value.Kind == DateTimeKind.Utc
            ? value
            : DateTime.SpecifyKind(value, DateTimeKind.Local).ToUniversalTime();

    // ── 服务器时区偏移（站点里手工填的优先，其次进程内探测到的）────────────────
    private int? _serverOffset;
    private bool _offsetProbed;

    private int? ServerOffsetMinutes => _config.ServerUtcOffsetMinutes ?? _serverOffset;

    /// <inheritdoc />
    public int? ProbeServerUtcOffset()
    {
        if (_config.ServerUtcOffsetMinutes.HasValue) return _config.ServerUtcOffsetMinutes;
        if (_offsetProbed) return _serverOffset;
        _offsetProbed = true;
        _serverOffset = ProbeTimeZoneFile();
        if (_serverOffset.HasValue) ServerOffsetStore.Persist(_config, _serverOffset.Value);
        return _serverOffset;
    }

    private void EnsureServerOffset() => ProbeServerUtcOffset();

    /// <summary>
    /// SFTP 协议本身不传服务器时区。Debian/Ubuntu（含 WSL）的 /etc/timezone 是纯文本 IANA 名，
    /// .NET 6+ 在 Windows 上也能直接解析 IANA id。取不到就返回 null —— 由用户在站点里手工填。
    /// 注意：SFTP 的时间正确性**不依赖**这个值（协议给的是 Unix 秒，绝对时刻），
    /// 它只影响「按服务器时区显示」这一口径。
    /// </summary>
    private int? ProbeTimeZoneFile()
    {
        EnsureConnected();
        if (_client is null) return null;
        try
        {
            using var buffer = new MemoryStream();
            _client.DownloadFile("/etc/timezone", buffer);
            var name = System.Text.Encoding.UTF8.GetString(buffer.ToArray()).Trim();
            if (string.IsNullOrWhiteSpace(name)) return null;
            return (int)TimeZoneInfo.FindSystemTimeZoneById(name).GetUtcOffset(DateTime.UtcNow).TotalMinutes;
        }
        catch { return null; }
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
    public void Download(string remotePath, string localPath, Action<long, long>? progress = null, bool resume = false,
                         CancellationToken token = default)
    {
        token.ThrowIfCancellationRequested();
        EnsureConnected();
        if (_client is null) return;
        long total = 0;
        try { total = _client.GetAttributes(remotePath).Size; } catch { }
        // 本地临时文件必须以**共享读**方式打开：Shell 扩展在下载进行中就要读它
        // （边下边读）。File.Create 默认 FileShare.None，扩展侧会拿到
        // ERROR_SHARING_VIOLATION(32)，表现为"磁盘操作失败"。
        using (var fs = new FileStream(localPath, FileMode.Create, FileAccess.Write,
                                       FileShare.Read | FileShare.Write | FileShare.Delete))
        {
            try
            {
                // 取消走 API 自己的 CancellationToken，**绝不能**在进度回调里抛异常：
                // SSH.NET 的同步 DownloadFile 把回调丢到线程池执行（内部 ThreadPoolProgress
                // + 写死 CancellationToken.None），回调里抛 OperationCanceledException 会变成
                // 线程池未处理异常、直接终止常驻服务进程（2026-09-20 用户实测："点取消后服务
                // 程序崩溃"），而且那个回调也取消不了下载、只会炸进程。
                // 异步重载把 token 交给 SFTP 请求本身：取消时 ReadAsync/WriteAsync 抛出的 OCE
                // 回到 GetResult() 的调用线程，由上层按"用户取消"处理；进度回调只负责上报
                // （以及服务侧的暂停闸门），经 SafeProgress 包装后永不抛出。
                IProgress<DownloadFileProgressReport>? reporter = progress is null
                    ? null
                    : new SafeProgress<DownloadFileProgressReport>(r => progress((long)r.TotalBytesDownloaded, total));
                _client.DownloadFileAsync(remotePath, fs, reporter, token).GetAwaiter().GetResult();
            }
            catch
            {
                // 中断/失败：删掉半截文件，别让它被当成"下载完成"
                try { fs.Dispose(); File.Delete(localPath); } catch { }
                throw;
            }
        }
        progress?.Invoke(new FileInfo(localPath).Length, new FileInfo(localPath).Length);
        Utils.ShellLog.Write($"SFTP get: {remotePath} -> {localPath}");
    }

    /// <summary>直传流：把远程文件**直接**写进给定的输出流（服务侧往命名管道里吐，
    /// 不再落本地临时文件）。目标文件由 Explorer 直接写 —— 省掉一次完整拷贝和一份磁盘占用。</summary>
    public void DownloadToStream(string remotePath, Stream output, Action<long, long>? progress = null,
                                 CancellationToken token = default)
    {
        token.ThrowIfCancellationRequested();
        EnsureConnected();
        if (_client is null) return;
        long total = 0;
        try { total = _client.GetAttributes(remotePath).Size; } catch { }
        IProgress<DownloadFileProgressReport>? reporter = progress is null
            ? null
            : new SafeProgress<DownloadFileProgressReport>(r => progress((long)r.TotalBytesDownloaded, total));
        _client.DownloadFileAsync(remotePath, output, reporter, token).GetAwaiter().GetResult();
        Utils.ShellLog.Write($"SFTP stream: {remotePath}");
    }

    public void Upload(string localPath, string remotePath, Action<long, long>? progress = null, bool resume = false,
                       CancellationToken token = default, Action? waitWhilePaused = null)
    {
        token.ThrowIfCancellationRequested();
        EnsureConnected();
        if (_client is null) return;
        long total = 0;
        try { total = new FileInfo(localPath).Length; } catch { }
        // 暂停/取消都放在**输入流**上：上传循环在自己的线程上 await input.ReadAsync(...)，
        // 在那里阻塞只会暂停这次上传；放到进度回调里会卡死会话消息线程（见 PausableReadStream）。
        using Stream fs = new PausableReadStream(File.OpenRead(localPath), waitWhilePaused);
        // 与 Download 同理：同步 UploadFile 把回调丢到线程池执行、写死 CancellationToken.None，
        // 回调里抛异常会终止宿主进程。改走带 token 的异步重载；进度回调经 SafeProgress 永不抛出。
        IProgress<UploadFileProgressReport>? reporter = progress is null
            ? null
            : new SafeProgress<UploadFileProgressReport>(r => progress((long)r.TotalBytesUploaded, total));
        _client.UploadFileAsync(fs, remotePath, true /* canOverride：与旧行为一致 */, reporter, token)
               .GetAwaiter().GetResult();
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
        // SFTP 协议（SSH_FXP_SETSTAT）只能写数字 uid/gid，所以**名字必须自己解析**：
        // 用 /etc/passwd、/etc/group 反查（与列里显示的 Owner/Group 用的是同一张缓存表）。
        // 纯数字一律原样透传：root 可以指定一个在 passwd 里还没有名字的 ID。
        var attrs = _client.GetAttributes(path)
            ?? throw new InvalidOperationException($"No such file: {path}");
        bool changed = false;
        if (!string.IsNullOrEmpty(user))
        {
            attrs.UserId = (int)ResolveOwnerId(user!, isUser: true);
            changed = true;
        }
        if (!string.IsNullOrEmpty(group))
        {
            attrs.GroupId = (int)ResolveOwnerId(group!, isUser: false);
            changed = true;
        }
        if (changed) _client.SetAttributes(path, attrs);
        Utils.ShellLog.Write($"SFTP chown: {path} user={user} group={group}");
    }

    /// <summary>
    /// 名字或数字 → uid/gid。数字直接返回；名字用远端 /etc/passwd、/etc/group 反查，
    /// 查不到就抛出**可读**错误（而不是丢一个 "UID must be numeric"）。
    /// </summary>
    private uint ResolveOwnerId(string token, bool isUser)
    {
        if (uint.TryParse(token, out uint numeric)) return numeric;

        EnsureIdMaps();                                  // 首次调用时拉 /etc/passwd、/etc/group
        var map = isUser ? _userNames : _groupNames;
        if (map is not null)
        {
            foreach (var kv in map)
                if (string.Equals(kv.Value, token, StringComparison.Ordinal))
                    return (uint)kv.Key;
        }
        throw new InvalidOperationException(isUser
            ? $"没有名为 '{token}' 的用户（可改填数字 UID，如 1000）"
            : $"没有名为 '{token}' 的组（可改填数字 GID，如 1000）");
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
