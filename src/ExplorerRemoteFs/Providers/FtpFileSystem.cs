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
        else
        {
            // FluentFTP 默认 Auto 会先尝试 AUTH TLS；纯 FTP 服务器（如 pyftpdlib 无 TLS）
            // 握手会失败。显式设为 None 走明文。
            _client.Config.EncryptionMode = FtpEncryptionMode.None;
        }
        // Many Unix FTP servers store Chinese names as UTF-8 but do not advertise
        // UTF8 in FEAT. The site setting deliberately overrides auto-detection.
        if (_config.FtpUseUtf8) _client.Encoding = System.Text.Encoding.UTF8;
        _client.Connect();
        Utils.ShellLog.Write($"FTP connected: {DisplayName}; encoding={_client.Encoding.WebName}");
    }

    public IReadOnlyList<RemoteEntry> List(string path)
    {
        EnsureConnected();
        if (_client is null) return Array.Empty<RemoteEntry>();

        var result = new List<RemoteEntry>();
        FtpListItem[] items;
        try
        {
            // ForceList：MLSD 不携带 unix.mode/owner/group fact，必须用 LIST 才能拿到 Unix 权限。
            // AllFiles：FluentFTP 默认过滤 . 开头的隐藏文件；FTP 服务器（如 pyftpdlib）LIST 会
            // 返回它们，Linux 语义下必须显示（与 WSL \\wsl$ 行为一致）。
            items = _client.GetListing(path, FtpListOption.ForceList | FtpListOption.AllFiles);
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

    public void Delete(string path)
    {
        EnsureConnected();
        if (_client is null) return;
        // Idempotent delete: Explorer deletes a directory recursively (single
        // DELETE recursive=1) and THEN, as a side channel of IFileOperation,
        // removes each child individually.  By the time those per-child requests
        // arrive, the entry is already gone — FluentFTP's DeleteFile would throw
        // a 550 and surface as a bogus "file delete failed" dialog.  A missing
        // target on a delete is "already deleted", i.e. success.  Only propagate
        // a real error (auth / permission / IO), never the already-gone case.
        // Files are probed first: the post-recursive-delete per-child payloads
        // queued by IFileOperation are overwhelmingly individual files.
        if (_client.FileExists(path)) { _client.DeleteFile(path); Utils.ShellLog.Write($"FTP deleted: {path}"); return; }
        if (_client.DirectoryExists(path)) { _client.DeleteDirectory(path); Utils.ShellLog.Write($"FTP deleted: {path}"); return; }
        Utils.ShellLog.Write($"FTP delete: already gone, treated as success: {path}");
    }

    public void Rename(string from, string to)
    {
        EnsureConnected();
        if (_client is null) return;
        _client.Rename(from, to);
        Utils.ShellLog.Write($"FTP renamed: {from} -> {to}");
    }

    public void CreateDirectory(string path)
    {
        EnsureConnected();
        if (_client is null) return;
        _client.CreateDirectory(path);
        Utils.ShellLog.Write($"FTP mkdir: {path}");
    }

    public void CreateEmptyFile(string path)
    {
        EnsureConnected();
        if (_client is null) return;
        // FTP has no O_EXCL equivalent.  FluentFTP's Skip mode performs the
        // best available no-overwrite operation; a simultaneous competing
        // creator still wins safely because we never request Overwrite.
        var local = Path.GetTempFileName();
        try
        {
            // UploadFile has the stable FtpStatus return contract across the
            // FluentFTP version used by this product; UploadStream's return
            // type changed between major releases.
            var result = _client.UploadFile(local, path, FtpRemoteExists.Skip, true, FtpVerify.None, null);
            if (result != FtpStatus.Success)
                throw new InvalidOperationException($"Remote file already exists or could not be created: {path}");
            Utils.ShellLog.Write($"FTP touch: {path}");
        }
        finally
        {
            try { File.Delete(local); } catch { }
        }
    }

    public void Download(string remotePath, string localPath, Action<long, long>? progress = null, bool resume = false)
    {
        EnsureConnected();
        if (_client is null) return;
        Action<FtpProgress>? fp = null;
        if (progress is not null)
            fp = p =>
            {
                // FluentFTP exposes no TotalBytes; derive it from the percentage.
                long total = p.Progress > 0 ? (long)(p.TransferredBytes / (p.Progress / 100.0)) : 0;
                progress(p.TransferredBytes, total);
            };
        // Resume uses the FTP REST command (FluentFTP handles it): the server
        // continues from the existing local file's size. Only requested when we
        // know a previous attempt of this transfer was interrupted.
        var mode = resume ? FtpLocalExists.Resume : FtpLocalExists.Overwrite;
        var result = _client.DownloadFile(localPath, remotePath, mode, FtpVerify.None, fp);
        if (result == FtpStatus.Failed)
            throw new InvalidOperationException($"FTP download failed: {remotePath}");
        progress?.Invoke(new FileInfo(localPath).Length, new FileInfo(localPath).Length);
        Utils.ShellLog.Write($"FTP get: {remotePath} -> {localPath}");
    }

    public void Upload(string localPath, string remotePath, Action<long, long>? progress = null, bool resume = false)
    {
        EnsureConnected();
        if (_client is null) return;
        long total = 0;
        try { total = new FileInfo(localPath).Length; } catch { }
        Action<FtpProgress>? fp = null;
        if (progress is not null)
            fp = p => progress(p.TransferredBytes, total);
        // FtpRemoteExists.Resume makes FluentFTP issue REST and continue from
        // the remote file's current size.
        var mode = resume ? FtpRemoteExists.Resume : FtpRemoteExists.Overwrite;
        var result = _client.UploadFile(localPath, remotePath, mode, true, FtpVerify.None, fp);
        if (result == FtpStatus.Failed)
            throw new InvalidOperationException($"FTP upload failed: {remotePath}");
        progress?.Invoke(total, total);
        Utils.ShellLog.Write($"FTP put: {localPath} -> {remotePath}");
    }

    public void SetPermissions(string path, int mode)
    {
        EnsureConnected();
        if (_client is null) return;
        // Some servers (e.g. pyftpdlib) expect "SITE CHMOD <mode> <path>" — mode FIRST.
        // FluentFTP's SetFilePermissions may emit the opposite order and get 550.
        string modeStr = Convert.ToString(mode, 8).PadLeft(3, '0');
        // Servers disagree on argument order: try "<mode> <path>" first (pyftpdlib),
        // fall back to "<path> <mode>" (some vsftpd-style servers).
        var resp = _client.Execute($"SITE CHMOD {modeStr} {path}");
        if (IsError(resp))
            resp = _client.Execute($"SITE CHMOD {path} {modeStr}");
        if (IsError(resp))
            throw new InvalidOperationException($"SITE CHMOD failed ({resp.Code} {resp.Message})");
        Utils.ShellLog.Write($"FTP chmod: {path} = {modeStr}");
    }

    public void SetOwner(string path, string? user, string? group)
    {
        // FTP 协议没有标准的 chown/chgrp；SITE CHOWN 只有极少数服务器实现。
        throw new InvalidOperationException("FTP does not support changing owner/group (SFTP only)");
    }

    private static bool IsError(FluentFTP.FtpReply resp)
        => resp.Code is { } c && (c.StartsWith("4") || c.StartsWith("5"));

    public ChmodRecursiveResult SetPermissionsRecursive(string path, int mode)
    {
        var failures = new List<string>();
        int dirs = 0, files = 0;

        // 起点本身也在范围内（chmod -R 的语义包含根目录）。
        try { SetPermissions(path, mode); dirs++; }
        catch (Exception ex) { failures.Add($"{path}: {ex.Message}"); }

        ChmodWalk(path, mode, failures, ref dirs, ref files);
        Utils.ShellLog.Write($"FTP chmod -R: {path} mode={Convert.ToString(mode, 8)} dirs={dirs} files={files} failed={failures.Count}");
        return new ChmodRecursiveResult(dirs, files, failures);
    }

    // 与 SFTP 实现同形：目录与文件都改，符号链接跳过，单项失败只记录。
    // FTP 的 SITE CHMOD 支持度因服务器而异，失败清单因此尤其重要——
    // 用户必须能看出"哪些条目没改成"，而不是拿到一个笼统的成功。
    private void ChmodWalk(string path, int mode, List<string> failures, ref int dirs, ref int files)
    {
        IReadOnlyList<RemoteEntry> entries;
        try { entries = List(path); }
        catch (Exception ex) { failures.Add($"{path}: {ex.Message}"); return; }

        foreach (var e in entries)
        {
            if (e.IsSymlink) continue;
            if (e.IsDirectory)
            {
                // 同 SFTP：目录自身改成与否都要继续下探，否则一棵子树会被静默跳过。
                try { SetPermissions(e.Path, mode); dirs++; }
                catch (Exception ex) { failures.Add($"{e.Path}: {ex.Message}"); }
                ChmodWalk(e.Path, mode, failures, ref dirs, ref files);
            }
            else
            {
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
        bool dl = _client.DownloadStream(ms, from, 0, null, 0);
        if (!dl) throw new InvalidOperationException($"FTP download failed: {from}");
        ms.Position = 0;
        _client.UploadStream(ms, to, FtpRemoteExists.Overwrite, true, null);
        Utils.ShellLog.Write($"FTP dup: {from} -> {to}");
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
