using System.IO;
using System.IO.Pipes;
using System.Text;
using ExplorerRemoteFs.Config;
using ExplorerRemoteFs.Providers;

namespace RemoteFsClient.Services;

/// <summary>
/// 本地命名管道桥接。ProviderFactory 位于常驻进程，目录浏览可复用 SFTP/FTP 会话。
/// 请求为三行 UTF-8：LIST、站点名、远程路径；响应沿用 CLI 的 ITEM\t... 格式。
/// </summary>
public sealed class RemoteBridgeService : IDisposable
{
    public const string PipeName = "ExplorerRemoteFs.Bridge.v1";
    private readonly Func<ErfNavigationRequest, Task>? _navigationHandler;
    private readonly RemoteOperationQueueService? _operationQueue;
    private readonly RemoteStatusService? _status;
    private readonly CancellationTokenSource _stop = new();
    private readonly SemaphoreSlim _providerGate = new(1, 1);
    private readonly object _listingCacheGate = new();
    private readonly Dictionary<string, ListingCacheEntry> _listingCache = new(StringComparer.Ordinal);
    private Task? _listener;
    private static readonly TimeSpan CompletedListingCacheLifetime = TimeSpan.FromSeconds(3);

    public RemoteBridgeService(Func<ErfNavigationRequest, Task>? navigationHandler = null,
                               RemoteOperationQueueService? operationQueue = null,
                               RemoteStatusService? status = null)
    {
        _navigationHandler = navigationHandler;
        _operationQueue = operationQueue;
        _status = status;
    }

    public void Start() => _listener ??= Task.WhenAll(
        Enumerable.Range(0, 4).Select(_ => Task.Run(() => ListenAsync(_stop.Token))));

    private async Task ListenAsync(CancellationToken token)
    {
        while (!token.IsCancellationRequested)
        {
            try
            {
                // Explorer may ask for the target directory and its item data in
                // parallel. Keep several pipe instances available; backend I/O is
                // still serialized below because a provider session is not thread-safe.
                await using var pipe = new NamedPipeServerStream(PipeName, PipeDirection.InOut, 4,
                    PipeTransmissionMode.Byte, PipeOptions.Asynchronous);
                await pipe.WaitForConnectionAsync(token);
                await HandleRequestAsync(pipe, token);
            }
            catch (OperationCanceledException) when (token.IsCancellationRequested) { break; }
            catch { }
        }
    }

    private async Task HandleRequestAsync(Stream stream, CancellationToken token)
    {
        using var reader = new StreamReader(stream, new UTF8Encoding(false), false, 4096, leaveOpen: true);
        await using var writer = new StreamWriter(stream, new UTF8Encoding(false), 4096, leaveOpen: true) { AutoFlush = true };
        string? operation = await reader.ReadLineAsync(token);

        // ERF-NAVIGATE is deliberately handled by the resident process.  The
        // short-lived URL-protocol process only forwards the original Explorer
        // HWND and exits; it must not create providers or open its own window.
        if (string.Equals(operation, "ERF-NAVIGATE", StringComparison.Ordinal))
        {
            var sourceWindowText = await reader.ReadLineAsync(token);
            var address = await reader.ReadLineAsync(token);
            if (_navigationHandler is null ||
                !long.TryParse(sourceWindowText, out var sourceWindow) ||
                string.IsNullOrWhiteSpace(address))
            {
                await writer.WriteLineAsync("FAIL: invalid ERF navigation request");
                return;
            }

            try
            {
                // Resolve the complete directory chain before touching the
                // Explorer window.  GetListingAsync first uses the resident
                // cache and only asks the remote provider for a cache miss.
                // The warmed responses are then reused by NSE PIDL parsing.
                await WarmErfNavigationAsync(address);
                await _navigationHandler(new ErfNavigationRequest(address, sourceWindow));
            }
            catch (Exception ex)
            {
                await writer.WriteLineAsync($"FAIL: {ex.Message.Replace('\r', ' ').Replace('\n', ' ')}");
                return;
            }
            await writer.WriteLineAsync("QUEUED");
            return;
        }

        // CACHE-CLEAR: the Explorer extension invalidates its own metadata
        // cache after a successful remote mutation and asks the bridge to drop
        // the matching listing cache too — otherwise a refresh right after an
        // operation would still serve the stale listing from here.
        // Read only the site line (the 2-line form is the canonical one); do
        // NOT wait for a 3rd line here — that would deadlock with the old DLL.
        if (string.Equals(operation, "CACHE-CLEAR", StringComparison.Ordinal))
        {
            string? site = await reader.ReadLineAsync(token);
            ClearListingCache(site);
            await writer.WriteLineAsync("OK");
            return;
        }

        // DELETE is deliberately synchronous at this boundary: IFileOperation
        // must not be told an item is deleted until the remote service has
        // actually completed (or failed) the mutation.  The work itself runs
        // on the resident service and reuses its ProviderFactory session.
        if (string.Equals(operation, "DELETE", StringComparison.Ordinal))
        {
            string? site = await reader.ReadLineAsync(token);
            string? deleteRemotePath = await reader.ReadLineAsync(token);
            string? recursiveText = await reader.ReadLineAsync(token);
            if (string.IsNullOrWhiteSpace(site) || string.IsNullOrWhiteSpace(deleteRemotePath) ||
                (recursiveText is not "0" and not "1"))
            {
                await writer.WriteLineAsync("FAIL: invalid delete request");
                return;
            }

            var result = await DeleteAsync(site, deleteRemotePath, recursiveText == "1");
            _status?.ReportSite(site, result.StartsWith("OK", StringComparison.Ordinal));   // R 字母颜色
            await writer.WriteLineAsync(result);
            return;
        }

        // CHMOD：递归修改权限。与 DELETE 同形——桥接边界同步等待，但真正的遍历跑在
        // 常驻服务里（自带进度窗口与「取消」）。此前 Explorer 属性页在 UI 线程同步等
        // CLI，树一大就卡死，而且 RunCli 的 30 秒超时会把 CLI 直接杀掉。
        if (string.Equals(operation, "CHMOD", StringComparison.Ordinal))
        {
            string? chmodSite = await reader.ReadLineAsync(token);
            string? chmodPath = await reader.ReadLineAsync(token);
            string? modeText = await reader.ReadLineAsync(token);
            string? chmodRecursiveText = await reader.ReadLineAsync(token);
            // 模式串是**八进制**（与 CLI 的 `chmod <site> <path> <mode>` 约定一致，
            // C++ 侧用 %03o 格式化）。按十进制解析会把 700 变成 0o1274 —— 实测会把目录
            // 改成 d-w-rwxr-T 并让后续遍历 Permission denied。
            int chmodMode = 0;
            bool modeOk = !string.IsNullOrWhiteSpace(modeText);
            if (modeOk)
            {
                try { chmodMode = Convert.ToInt32(modeText!.Trim(), 8); }
                catch { modeOk = false; }
            }
            if (string.IsNullOrWhiteSpace(chmodSite) || string.IsNullOrWhiteSpace(chmodPath) ||
                !modeOk || chmodMode <= 0 || chmodMode > 0xFFF ||
                (chmodRecursiveText is not "0" and not "1"))
            {
                await writer.WriteLineAsync("FAIL: invalid chmod request");
                return;
            }
            var chmodResult = await ChmodAsync(chmodSite, chmodPath, chmodMode, chmodRecursiveText == "1");
            _status?.ReportSite(chmodSite, chmodResult.StartsWith("OK", StringComparison.Ordinal));
            await writer.WriteLineAsync(chmodResult);
            return;
        }

        string? siteName = await reader.ReadLineAsync(token);
        string? path = await reader.ReadLineAsync(token);
        if (!string.Equals(operation, "LIST", StringComparison.Ordinal) || string.IsNullOrWhiteSpace(siteName))
        {
            await writer.WriteLineAsync("FAIL: invalid bridge request");
            return;
        }

        var remotePath = path ?? string.Empty;
        try
        {
            var response = await GetListingAsync(siteName, remotePath);
            _status?.ReportSite(siteName, true);      // 能列出内容 = 该站点连接正常
            await writer.WriteAsync(response);
        }
        catch (Exception ex)
        {
            await writer.WriteLineAsync($"FAIL: {ex.Message.Replace('\r', ' ').Replace('\n', ' ')}");
            _status?.ReportSite(siteName, false);   // 列不出来 = 该站点连接不正常（R 变红）
            if (!string.IsNullOrWhiteSpace(siteName)) ProviderFactory.Invalidate(siteName);
        }
    }

    private void ClearListingCache(string? siteName)
    {
        lock (_listingCacheGate)
        {
            if (string.IsNullOrWhiteSpace(siteName) || siteName == "*")
            {
                _listingCache.Clear();
                return;
            }
            var prefix = siteName.ToUpperInvariant() + "\0";
            foreach (var key in _listingCache.Keys.Where(k => k.StartsWith(prefix, StringComparison.Ordinal)).ToArray())
                _listingCache.Remove(key);
        }
    }

    // Explorer can ask for the same directory concurrently while it binds a
    // target and prepares its view.  Keep completed responses briefly as well:
    // ERF preflight warms every path component before the original Explorer
    // window navigates.  Remote mutations send CACHE-CLEAR, so a confirmed
    // write never leaves this cache authoritative.
    private Task<string> GetListingAsync(string siteName, string remotePath)
    {
        var key = siteName.ToUpperInvariant() + "\0" + remotePath;
        lock (_listingCacheGate)
        {
            var now = DateTimeOffset.UtcNow;
            foreach (var expired in _listingCache.Where(pair => pair.Value.ExpiresAt <= now).Select(pair => pair.Key).ToArray())
                _listingCache.Remove(expired);

            if (_listingCache.TryGetValue(key, out var existing))
                return existing.Response; // completed hit or in-flight coalescing

            var task = BuildListingAsync(siteName, remotePath);
            _listingCache[key] = new ListingCacheEntry(task, now + CompletedListingCacheLifetime);
            _ = task.ContinueWith(t =>
            {
                lock (_listingCacheGate)
                {
                    if (_listingCache.TryGetValue(key, out var cur) && ReferenceEquals(cur.Response, task) &&
                        (t.IsFaulted || t.IsCanceled || (t.Status == TaskStatus.RanToCompletion && t.Result.StartsWith("FAIL:", StringComparison.Ordinal))))
                        _listingCache.Remove(key); // failures are never cached
                }
            }, TaskScheduler.Default);
            return task;
        }
    }

    private async Task<string> DeleteAsync(string siteName, string requestedPath, bool recursive)
    {
        using var cancellation = CancellationTokenSource.CreateLinkedTokenSource(_stop.Token);
        OperationHandle? progress = null;
        bool gateHeld = false;
        try
        {
            var connection = FindConnection(siteName);
            var root = NormalizeRemotePath(connection.StartPath);
            var path = NormalizeRemotePath(requestedPath);
            if (!IsAtOrBelow(path, root) || path == root)
                return "FAIL: refusing to delete the configured site root or a path outside it";

            progress = _operationQueue?.Begin(siteName, path,
                RemoteOperationQueueWindow.OperationKind.Delete,
                () => { try { cancellation.Cancel(); } catch (ObjectDisposedException) { } });
            progress?.Update(0, 0, Ui.IsEnglish ? "Waiting for remote connection" : "等待远程连接");

            await _providerGate.WaitAsync(cancellation.Token);
            gateHeld = true;
            var fs = ProviderFactory.Get(connection);
            await Task.Run(() => ExecuteDelete(fs, path, recursive, cancellation.Token, progress), cancellation.Token);
            ClearListingCache(siteName);
            progress?.Complete(true, false, null);
            return "OK";
        }
        catch (OperationCanceledException)
        {
            progress?.Complete(false, true, null);
            return "FAIL: cancelled";
        }
        catch (Exception ex)
        {
            ProviderFactory.Invalidate(siteName);
            progress?.Complete(false, false, ex.Message);
            return $"FAIL: {SanitizeBridgeError(ex.Message)}";
        }
        finally
        {
            if (gateHeld) _providerGate.Release();
        }
    }

    /// <summary>递归（或单个）修改远程权限：与删除同构——自带进度窗口、可取消，
    /// 完成后清掉该站点的目录缓存，并把结果回给 Explorer（OK / FAIL: …）。</summary>
    private async Task<string> ChmodAsync(string siteName, string requestedPath, int mode, bool recursive)
    {
        using var cancellation = CancellationTokenSource.CreateLinkedTokenSource(_stop.Token);
        OperationHandle? progress = null;
        bool gateHeld = false;
        try
        {
            var connection = FindConnection(siteName);
            var root = NormalizeRemotePath(connection.StartPath);
            var path = NormalizeRemotePath(requestedPath);
            if (!IsAtOrBelow(path, root))
                return "FAIL: refusing to change permissions outside the configured site root";
            if (path == root && !recursive)
                return "FAIL: refusing to change the configured site root itself";

            progress = _operationQueue?.Begin(siteName, path,
                RemoteOperationQueueWindow.OperationKind.Chmod,
                () => { try { cancellation.Cancel(); } catch (ObjectDisposedException) { } });
            progress?.Update(0, 0, Ui.IsEnglish ? "Waiting for remote connection" : "等待远程连接");

            await _providerGate.WaitAsync(cancellation.Token);
            gateHeld = true;
            var fs = ProviderFactory.Get(connection);

            long seen = 0;
            DateTime lastUi = DateTime.MinValue;
            void OnItem(string current)
            {
                seen++;
                var now = DateTime.UtcNow;
                // 回报速率刻意压低（约 3 次/秒）：进度显示对后端只是开销，
                // 大树上百万条目时高频回报会让 UI 与遍历线程互相抢时间。
                if (now - lastUi < TimeSpan.FromMilliseconds(300)) return;
                lastUi = now;
                progress?.Update(seen, 0, current);
            }

            var result = await Task.Run(() => fs.SetPermissionsRecursive(path, mode, OnItem, cancellation.Token),
                                        cancellation.Token);

            ClearListingCache(siteName);
            if (result.Partial)
            {
                // 部分失败必须让用户看见，不能在进度窗口上显示"完成"了事。
                string detail = string.Join("; ", result.Failures.Take(5));
                if (result.Failures.Count > 5) detail += $" (+{result.Failures.Count - 5})";
                progress?.Complete(false, false,
                    Ui.IsEnglish
                        ? $"{result.Dirs} dirs / {result.Files} files updated, {result.Failures.Count} failed: {detail}"
                        : $"已修改 {result.Dirs} 个目录 / {result.Files} 个文件，{result.Failures.Count} 项失败：{detail}");
                return $"FAIL: partial ({result.Failures.Count} failed) {detail}";
            }
            progress?.Complete(true, false, null);
            return "OK";
        }
        catch (OperationCanceledException)
        {
            progress?.Complete(false, true, null);
            return "FAIL: cancelled";
        }
        catch (Exception ex)
        {
            ProviderFactory.Invalidate(siteName);
            progress?.Complete(false, false, ex.Message);
            return $"FAIL: {SanitizeBridgeError(ex.Message)}";
        }
        finally
        {
            if (gateHeld) _providerGate.Release();
        }
    }

    private void ExecuteDelete(IRemoteFileSystem fs, string path, bool recursive,
                               CancellationToken cancellation, OperationHandle? progress)
    {
        var plan = new List<DeletePlanItem>();
        var lastUiUpdate = DateTime.MinValue;
        void Report(long done, long total, string current, bool force = false)
        {
            var now = DateTime.UtcNow;
            // A tree can contain millions of entries.  Reporting every delete
            // would enqueue millions of Dispatcher work items and make the
            // control centre itself the bottleneck; 5 updates/sec is enough
            // for visible progress while the final item is always reported.
            if (!force && now - lastUiUpdate < TimeSpan.FromMilliseconds(200)) return;
            lastUiUpdate = now;
            progress?.Update(done, total, current);
        }
        if (recursive)
            BuildDeletePlan(fs, path, plan, cancellation, item =>
                Report(0, 0, Ui.IsEnglish ? "Scanning " + item : "正在扫描 " + item));
        else
            plan.Add(new DeletePlanItem(path, false));

        Report(0, plan.Count, Ui.IsEnglish ? "Deleting" : "正在删除", force: true);
        long completed = 0;
        foreach (var item in plan)
        {
            cancellation.ThrowIfCancellationRequested();
            Report(completed, plan.Count, item.Path);
            fs.Delete(item.Path);
            completed++;
            Report(completed, plan.Count, item.Path, force: completed == plan.Count);
        }
    }

    // Delete children before their parent.  Symlinks are leaf objects even if
    // their targets happen to be directories; never recurse through them.
    private static void BuildDeletePlan(IRemoteFileSystem fs, string directory,
                                        List<DeletePlanItem> plan, CancellationToken cancellation,
                                        Action<string> scanning)
    {
        cancellation.ThrowIfCancellationRequested();
        scanning(directory);
        foreach (var entry in fs.List(directory))
        {
            cancellation.ThrowIfCancellationRequested();
            if (entry.IsDirectory && !entry.IsSymlink)
                BuildDeletePlan(fs, entry.Path, plan, cancellation, scanning);
            else
                plan.Add(new DeletePlanItem(entry.Path, false));
        }
        plan.Add(new DeletePlanItem(directory, true));
    }

    private static string SanitizeBridgeError(string message) =>
        (message ?? "remote delete failed").Replace('\r', ' ').Replace('\n', ' ');

    private sealed record DeletePlanItem(string Path, bool IsDirectory);

    private async Task WarmErfNavigationAsync(string address)
    {
        if (!TryParseErfAddress(address, out var siteName, out var requestedPath))
            throw new InvalidOperationException("Invalid ERF address. Expected erf:<site>:/absolute/unix/path.");

        var connection = FindConnection(siteName);
        var startPath = NormalizeRemotePath(connection.StartPath);
        var fullPath = NormalizeRemotePath(requestedPath);
        if (!IsAtOrBelow(fullPath, startPath))
            throw new InvalidOperationException($"The ERF path is outside the configured start path '{startPath}'.");

        var currentPath = startPath;
        var remainder = fullPath.Length == startPath.Length ? string.Empty : fullPath[startPath.Length..].TrimStart('/');
        foreach (var segment in remainder.Split('/', StringSplitOptions.RemoveEmptyEntries))
        {
            var listing = await GetListingAsync(siteName, currentPath);
            if (listing.StartsWith("FAIL:", StringComparison.Ordinal))
                throw new InvalidOperationException(listing[5..].Trim());
            if (!ListingContainsDirectory(listing, segment))
                throw new DirectoryNotFoundException($"Remote directory not found: {fullPath}");
            currentPath = currentPath == "/" ? "/" + segment : currentPath + "/" + segment;
        }

        // Prewarm the target view as well; the first Explorer enumeration can
        // consume this completed cache entry without another remote round trip.
        var targetListing = await GetListingAsync(siteName, currentPath);
        if (targetListing.StartsWith("FAIL:", StringComparison.Ordinal))
            throw new InvalidOperationException(targetListing[5..].Trim());
    }

    private static bool TryParseErfAddress(string address, out string siteName, out string remotePath)
    {
        siteName = string.Empty;
        remotePath = string.Empty;
        if (!address.StartsWith("erf:", StringComparison.OrdinalIgnoreCase)) return false;
        var target = address[4..];
        var separator = target.IndexOf(':');
        if (separator <= 0 || separator == target.Length - 1) return false;
        siteName = Uri.UnescapeDataString(target[..separator]);
        remotePath = Uri.UnescapeDataString(target[(separator + 1)..]);
        return siteName.IndexOfAny(['/', '\\', ':']) < 0 && remotePath.StartsWith('/') && !remotePath.Contains('\\');
    }

    private static string NormalizeRemotePath(string? path)
    {
        var normalized = string.IsNullOrWhiteSpace(path) ? "/" : path.Trim();
        if (!normalized.StartsWith('/')) normalized = "/" + normalized;
        while (normalized.Length > 1 && normalized.EndsWith('/')) normalized = normalized[..^1];
        return normalized;
    }

    private static bool IsAtOrBelow(string path, string root) =>
        root == "/" || path.Equals(root, StringComparison.Ordinal) ||
        (path.StartsWith(root, StringComparison.Ordinal) && path.Length > root.Length && path[root.Length] == '/');

    private static bool ListingContainsDirectory(string listing, string name)
    {
        foreach (var line in listing.Split('\n', StringSplitOptions.RemoveEmptyEntries))
        {
            var fields = line.TrimEnd('\r').Split('\t');
            if (fields.Length > 9 && fields[0] == "ITEM" && fields[6] == "1" && fields[9] == name)
                return true;
        }
        return false;
    }

    private async Task<string> BuildListingAsync(string siteName, string remotePath)
    {
        try
        {
            await _providerGate.WaitAsync(_stop.Token);
            try
            {
                var connection = FindConnection(siteName);
                if (string.IsNullOrWhiteSpace(remotePath)) remotePath = connection.StartPath;
                if (string.IsNullOrWhiteSpace(remotePath)) remotePath = "/";
                var text = new StringBuilder();
                var fs = ProviderFactory.Get(connection);
                foreach (var entry in fs.List(remotePath))
                {
                    var mode = string.IsNullOrEmpty(entry.ModeDisplay) ? (entry.IsDirectory ? "drwxr-xr-x" : "-rw-r--r--") : entry.ModeDisplay;
                    var mtime = entry.LastWriteTime.HasValue ? new DateTimeOffset(entry.LastWriteTime.Value.ToUniversalTime()).ToUnixTimeSeconds().ToString() : "0";
                    text.Append("ITEM\t").Append(mode).Append('\t').Append(mtime).Append('\t').Append(entry.Size).Append('\t')
                        .Append(entry.OwnerDisplay).Append('\t').Append(entry.GroupDisplay).Append('\t')
                        .Append(entry.IsDirectory ? 1 : 0).Append('\t').Append(entry.IsSymlink ? 1 : 0).Append('\t')
                        .Append(remotePath).Append('\t').Append(entry.Name).Append('\t').Append(entry.Uid).Append('\t').Append(entry.Gid).AppendLine();
                }
                text.AppendLine("BRIDGE-END");
                return text.ToString();
            }
            finally { _providerGate.Release(); }
        }
        catch (Exception ex)
        {
            if (!string.IsNullOrWhiteSpace(siteName)) ProviderFactory.Invalidate(siteName);
            return $"FAIL: {ex.Message.Replace('\r', ' ').Replace('\n', ' ')}\n";
        }
    }

    private static ConnectionConfig FindConnection(string name)
    {
        var connection = ConnectionStore.Load().FirstOrDefault(c => c.Name.Equals(name, StringComparison.OrdinalIgnoreCase));
        if (connection is null) throw new InvalidOperationException($"Connection '{name}' not found.");
        if (string.IsNullOrEmpty(connection.Password) && CredentialManager.TryRead(connection.Name, out _, out var secret)) connection.Password = secret;
        return connection;
    }

    public void Dispose()
    {
        _stop.Cancel();
        try { _listener?.Wait(TimeSpan.FromSeconds(1)); } catch { }
        _providerGate.Dispose();
        _stop.Dispose();
    }

    private sealed record ListingCacheEntry(Task<string> Response, DateTimeOffset ExpiresAt);
}

/// <summary>Navigation intent forwarded from the erf: protocol entry point to
/// the per-user resident service.  SourceWindowHandle is the root HWND of the
/// Explorer window in which the user entered the address.</summary>
public readonly record struct ErfNavigationRequest(string Address, long SourceWindowHandle);
