using System.IO;
using System.IO.Pipes;
using System.Text;
using ExplorerRemoteFs.Config;
using ExplorerRemoteFs.Providers;
using Microsoft.Data.Sqlite;
using Microsoft.Win32;

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
    private readonly TransferTaskService? _transfers;   // 传输队列（上传/下载），与操作队列分开
    private Task? _listener;
    private static readonly TimeSpan CompletedListingCacheLifetime = TimeSpan.FromSeconds(3);

    // ── 站点名大小写语义（2026-09-24）──────────────────────────────────────
    // WSL 与 wsl 是**两个不同的站点**。此前 FindConnection 用 OrdinalIgnoreCase、
    // 列表缓存键用 ToUpperInvariant —— 大小写同名并存时输入哪个都会串到另一个站点，
    // 且两站点的列表共享同一个缓存键（互相污染）。现在：
    //   1) 精确匹配优先；
    //   2) 无精确命中且大小写不敏感命中**恰好一个**时才回退（打 wsl 找到唯一的 WSL 仍可用）；
    //   3) ≥2 个不敏感命中 = 有歧义，按"找不到"处理，绝不猜。
    private static ConnectionConfig? FindConnection(string name, out string canonicalName)
    {
        canonicalName = name;
        var connections = ConnectionStore.Load();
        var exact = connections.FirstOrDefault(c => c.Name.Equals(name, StringComparison.Ordinal));
        if (exact is not null)
        {
            canonicalName = exact.Name;
            return WithCredentials(exact);
        }
        var insensitive = connections.Where(c => c.Name.Equals(name, StringComparison.OrdinalIgnoreCase)).ToList();
        if (insensitive.Count == 1)
        {
            canonicalName = insensitive[0].Name;
            return WithCredentials(insensitive[0]);
        }
        return null;
    }

    private static ConnectionConfig WithCredentials(ConnectionConfig connection)
    {
        if (string.IsNullOrEmpty(connection.Password) && CredentialManager.TryRead(connection.Name, out _, out var secret))
            connection.Password = secret;
        return connection;
    }

    public RemoteBridgeService(Func<ErfNavigationRequest, Task>? navigationHandler = null,
                               RemoteOperationQueueService? operationQueue = null,
                               RemoteStatusService? status = null,
                               TransferTaskService? transfers = null)
    {
        _navigationHandler = navigationHandler;
        _operationQueue = operationQueue;
        _status = status;
        _transfers = transfers;
    }

    public void Start() => _listener ??= Task.WhenAll(
        Enumerable.Range(0, 4).Select(_ => Task.Run(() => ListenAsync(_stop.Token))));

    private TransferTicketService? _tickets;

    /// <summary>票据服务在 App 里创建（它反过来要引用本对象来入队），随后挂到这里。</summary>
    public void AttachTickets(TransferTicketService tickets) => _tickets = tickets;

    /// <summary>票据路径：按"我们自己的队列任务"启动一次下载（因此真暂停可用）。
    /// 文件走 <c>&lt;目标&gt;.rfs-part</c> → 原子改名；目录下到目标目录下、以远程目录名为子目录。</summary>
    public void StartTicketDownload(string site, string remote, string target, bool isFolder, string batchId)
    {
        string local = target;
        bool atomic = false;
        if (isFolder)
            local = System.IO.Path.GetDirectoryName(target) ?? target;   // FetchDirAsync 会在其下建 <name>
        else
            atomic = true;
        var job = new FetchJob
        {
            Id = Guid.NewGuid().ToString("N"),
            Site = site, Remote = remote, Local = local, BatchId = batchId, AtomicLocal = atomic,
        };
        _fetchJobs[job.Id] = job;
        Log($"ticket download queued id='{job.Id}' site='{site}' remote='{remote}' target='{target}' kind={(isFolder ? "dir" : "file")}");
        _ = Task.Run(async () =>
        {
            try
            {
                FetchResult result = isFolder ? await FetchDirAsync(job) : await FetchAsync(job);
                job.Message = result.Message;
                Volatile.Write(ref job.State, (int)result.State);
                Log($"ticket download terminal id='{job.Id}' state={result.State} message='{result.Message}'");
            }
            catch (Exception ex)
            {
                job.Message = SanitizeBridgeError(ex.Message);
                Volatile.Write(ref job.State, (int)FetchJobState.Failed);
                Log($"ticket download unhandled id='{job.Id}' exception='{SanitizeBridgeError(ex.ToString())}'");
            }
            finally
            {
                _ = Task.Delay(TimeSpan.FromMinutes(5)).ContinueWith(t => _fetchJobs.TryRemove(job.Id, out var _));
            }
        });
    }

    private async Task ListenAsync(CancellationToken token)
    {
        while (!token.IsCancellationRequested)
        {
            try
            {
                // Explorer may ask for the target directory and its item data in
                // parallel. Keep several pipe instances available; backend I/O is
                // still serialized below because a provider session is not thread-safe.
                await using var pipe = new NamedPipeServerStream(PipeName, PipeDirection.InOut, 8,
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

        // FETCH：把远程文件下载到本地临时文件（复制 / 拖拽的下载方向）。
        //
        // 与 DELETE / CHMOD 完全同形，也是架构约束要求的形态：Shell DLL 只负责
        // Shell/PIDL/视图/菜单，会话、连接、缓存、传输队列都归常驻服务。
        // 以前是 Shell DLL 自己 CreateProcess(cli get) + WaitForSingleObject(INFINITE)：
        // 每个文件一个进程、一次全新登录，"一次复制 10 个文件"就是 10 个进程 10 次握手，
        // 而且复制期间 Explorer 全程卡着。现在复用 ProviderFactory 会话，下载进传输队列
        //（有进度、可取消），桥接边界只等最终 OK/FAIL。
        //
        // batchId 把"同一次用户复制操作"的所有文件归到一组，队列 UI 据此折叠显示。
        if (string.Equals(operation, "FETCH", StringComparison.Ordinal))
        {
            string? fetchSite = await reader.ReadLineAsync(token);
            string? fetchRemote = await reader.ReadLineAsync(token);
            string? fetchLocal = await reader.ReadLineAsync(token);
            string? fetchBatch = await reader.ReadLineAsync(token);
            if (string.IsNullOrWhiteSpace(fetchSite) || string.IsNullOrWhiteSpace(fetchRemote) ||
                string.IsNullOrWhiteSpace(fetchLocal))
            {
                await writer.WriteLineAsync("FAIL: invalid fetch request");
                return;
            }
            // 立即返回 jobId，下载在后台跑：绝不能让一个请求占着管道实例等到文件下完
            // （管道只有几个实例，占住就等于把 LIST 也堵死 —— 那正是"复制时更卡"的原因）。
            var fetchJob = StartFetchJob(fetchSite, fetchRemote, fetchLocal, fetchBatch ?? string.Empty, isDir: false);
            await writer.WriteLineAsync("STARTED " + fetchJob.Id);
            return;
        }

        // FETCHDIR：递归下载一个远程目录（拖拽/复制整个文件夹）。
        // 与 FETCH 同一套路，只是把"一个文件"换成"一棵树"：服务侧先扫一遍树拿到文件总数，
        // 再逐个下载并把进度聚合到一个队列条目上 —— 一个文件夹 = 一个任务（用户要求），
        // 同时拖多个文件夹时每个文件夹各自一组（扩展为每个顶层文件夹生成一个 batchId）。
        if (string.Equals(operation, "FETCHDIR", StringComparison.Ordinal))
        {
            string? dirSite = await reader.ReadLineAsync(token);
            string? dirRemote = await reader.ReadLineAsync(token);
            string? dirLocalRoot = await reader.ReadLineAsync(token);
            string? dirBatch = await reader.ReadLineAsync(token);
            if (string.IsNullOrWhiteSpace(dirSite) || string.IsNullOrWhiteSpace(dirRemote) ||
                string.IsNullOrWhiteSpace(dirLocalRoot))
            {
                await writer.WriteLineAsync("FAIL: invalid fetchdir request");
                return;
            }
            var dirJob = StartFetchJob(dirSite, dirRemote, dirLocalRoot, dirBatch ?? string.Empty, isDir: true);
            await writer.WriteLineAsync("STARTED " + dirJob.Id);
            return;
        }

        // PUT：把本地文件上传到远程（粘贴 / 编辑回写 / 跨站点复制的上传段）。
        // 与 FETCH 完全同形：立刻回 jobId，上传在后台跑，进传输队列（有进度、可取消）。
        if (string.Equals(operation, "PUT", StringComparison.Ordinal))
        {
            string? putSite = await reader.ReadLineAsync(token);
            string? putRemote = await reader.ReadLineAsync(token);
            string? putLocal = await reader.ReadLineAsync(token);
            string? putBatch = await reader.ReadLineAsync(token);
            if (string.IsNullOrWhiteSpace(putSite) || string.IsNullOrWhiteSpace(putRemote) ||
                string.IsNullOrWhiteSpace(putLocal))
            {
                await writer.WriteLineAsync("FAIL: invalid put request");
                return;
            }
            var putJob = StartFetchJob(putSite, putRemote, putLocal, putBatch ?? string.Empty, isDir: false, isPut: true);
            await writer.WriteLineAsync("STARTED " + putJob.Id);
            return;
        }

        // MKTICKET：扩展在 Ctrl+C 时登记"这次要传什么"。服务登记 jobId 并回它；
        // 票据文本（只有 magic/version/jobId）由扩展自己拼 —— 服务**不把路径写进票据**。
        if (string.Equals(operation, "MKTICKET", StringComparison.Ordinal))
        {
            string? tkSite = await reader.ReadLineAsync(token);
            string? tkCount = await reader.ReadLineAsync(token);
            if (_tickets is null || string.IsNullOrWhiteSpace(tkSite) ||
                !int.TryParse(tkCount, out int itemCount) || itemCount <= 0 || itemCount > 10000)
            {
                await writer.WriteLineAsync("FAIL: invalid mkticket request");
                return;
            }
            var items = new List<TicketItem>();
            for (int i = 0; i < itemCount; i++)
            {
                string? line = await reader.ReadLineAsync(token);
                if (line is null) break;
                var f = line.Split('\t');
                if (f.Length < 5) continue;
                items.Add(new TicketItem
                {
                    Site = tkSite!.Trim(),
                    Remote = f[0],
                    Name = f[1],
                    Size = long.TryParse(f[2], out long sz) ? sz : 0,
                    Mtime = long.TryParse(f[3], out long mt) ? mt : 0,
                    IsFolder = f[4] == "1",
                });
            }
            if (items.Count == 0) { await writer.WriteLineAsync("FAIL: no items"); return; }
            string jobId = _tickets.CreateTicket(items);
            await writer.WriteLineAsync("OK " + jobId);
            return;
        }

        // OPEN-TICKET：双击 .erfdl（或命令行）转到这里。**立刻回 OK**，解析/可能的询问/入队
        // 都放到后台 —— 否则会占着管道实例等用户点确认。
        if (string.Equals(operation, "OPEN-TICKET", StringComparison.Ordinal))
        {
            string? ticketPath = await reader.ReadLineAsync(token);
            if (_tickets is null || string.IsNullOrWhiteSpace(ticketPath))
            {
                await writer.WriteLineAsync("FAIL: invalid ticket");
                return;
            }
            string ticketFilePath = ticketPath!;
            await writer.WriteLineAsync("OK queued");
            _ = Task.Run(() => { string r = _tickets.OpenTicket(ticketFilePath); Log($"open-ticket result: {r}"); });
            return;
        }

        // FETCHSTREAM：把远程文件**直接以字节流**回给调用方（DLL 的 IStream），
        // 不再落 %TEMP% 临时文件 —— 目标文件由 Explorer 直接写，省掉一次完整拷贝
        // 和一份磁盘占用，失败/取消时半成品由 Explorer 自己删（等价 .part 语义）。
        // 协议：写 FETCHSTREAM/site/remote/batchId → 回一行 `OK <jobId>` → 随后同一
        // 管道上就是原始字节，直到服务关闭管道；失败在开头回 `FAIL: ...`。
        if (string.Equals(operation, "FETCHSTREAM", StringComparison.Ordinal))
        {
            string? stSite = await reader.ReadLineAsync(token);
            string? stRemote = await reader.ReadLineAsync(token);
            string? stBatch = await reader.ReadLineAsync(token);
            if (string.IsNullOrWhiteSpace(stSite) || string.IsNullOrWhiteSpace(stRemote))
            {
                await writer.WriteLineAsync("FAIL: invalid fetchstream request");
                return;
            }
            var stJob = new FetchJob
            {
                Id = Guid.NewGuid().ToString("N"),
                Site = stSite!, Remote = stRemote!, Local = "", BatchId = stBatch ?? string.Empty,
            };
            _fetchJobs[stJob.Id] = stJob;
            await writer.WriteLineAsync("OK " + stJob.Id);
            Log($"stream start id='{stJob.Id}' site='{stSite}' remote='{stRemote}' batch='{stBatch}'");
            await StreamFetchAsync(stJob, stream);
            _ = Task.Delay(TimeSpan.FromMinutes(5)).ContinueWith(t => _fetchJobs.TryRemove(stJob.Id, out var _));
            return;
        }

        // FETCHSTATUS：查询一个后台下载任务。短请求，秒回 —— 扩展侧靠它区分
        // "还在下" / "下完了" / "失败了" / "被用户取消了"。
        if (string.Equals(operation, "FETCHSTATUS", StringComparison.Ordinal))
        {
            var jobId = await reader.ReadLineAsync(token);
            if (jobId is not null && _fetchJobs.TryGetValue(jobId, out var job))
            {
                switch ((FetchJobState)Volatile.Read(ref job.State))
                {
                    case FetchJobState.Done: await writer.WriteLineAsync("DONE"); break;
                    case FetchJobState.Cancelled: await writer.WriteLineAsync("CANCELLED"); break;
                    case FetchJobState.Failed: await writer.WriteLineAsync("FAILED " + job.Message); break;
                    default: await writer.WriteLineAsync($"RUNNING {job.Done} {job.Total}"); break;
                }
            }
            else
            {
                await writer.WriteLineAsync("UNKNOWN");
            }
            return;
        }

        // CANCEL：让常驻服务**真的停下**同一批（batchId）还在跑的传输。
        // 触发点有两个：用户在传输队列里点「取消」；以及 Explorer 的复制对话框被取消
        //（我们的流没读完就被释放了）。只"不再读流"不够 —— 服务侧照样会把整棵树下完。
        if (string.Equals(operation, "CANCEL", StringComparison.Ordinal))
        {
            var cancelBatch = await reader.ReadLineAsync(token) ?? string.Empty;
            // The bridge owns the actual work item. Cancelling only the UI row is
            // racy: a just-started FETCH may not have posted its row yet.
            int cancelledJobs = CancelFetchJobs(cancelBatch);
            int cancelledTasks = _transfers?.CancelBatch(cancelBatch) ?? 0;
            Log($"cancel request batch='{cancelBatch}' jobs={cancelledJobs} queueRows={cancelledTasks}");
            await writer.WriteLineAsync("OK " + cancelledJobs);
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
            // 键不再折叠大小写：清除也按"已解析的规范站点名"算前缀。
            string canonical = siteName;
            try
            {
                if (FindConnection(siteName, out var resolved) is not null || !string.IsNullOrEmpty(resolved))
                    canonical = resolved;
            }
            catch { /* 解析失败就用原样前缀，清不到也不致命 */ }
            var prefix = canonical + "\0";
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
        // 键用**规范站点名**（配置里的拼写）：大小写同名站点绝不共享缓存条目。
        // 解析不到（不存在/歧义）时退回原名 —— 后续 BuildListingAsync 会产出 FAIL，
        // 失败从不缓存，所以这个键只是占位。
        string key;
        try
        {
            key = (FindConnection(siteName, out var canonical) is not null ? canonical : siteName) + "\0" + remotePath;
        }
        catch { key = siteName + "\0" + remotePath; }
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
                () => { try { cancellation.Cancel(); } catch { } });
            progress?.Update(0, 0, Ui.IsEnglish ? "Waiting for remote connection" : "等待远程连接");

            await _providerGate.WaitAsync(cancellation.Token);
            gateHeld = true;
            var fs = ProviderFactory.Get(connection);
            await Task.Run(() => ExecuteDelete(fs, path, recursive, cancellation.Token, progress), cancellation.Token);
            ClearListingCache(siteName);
            progress?.Complete(true, false, null);
            return "OK";
        }
        // 只有**我们自己的** token 被取消才算用户取消：SSH.NET 在超时/断连时
        // 也会抛 OperationCanceledException，那属于可重试的失败，不能回成 cancelled
        //（扩展侧对 cancelled 的语义是永久放弃、不再重试）。
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
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

    /// <summary>一个后台下载任务（单文件或整棵目录树）。
    /// 存在的理由：桥接请求必须**立刻**返回，不能占着管道实例等下载完。</summary>
    // FETCHSTATUS is a typed protocol boundary. Do not infer a user cancel from
    // an exception message: cancellation is a terminal state owned by the job.
    private enum FetchJobState { Running, Done, Failed, Cancelled }
    private readonly record struct FetchResult(FetchJobState State, string Message);

    private sealed class FetchJob
    {
        public string Id = "";
        public string Site = "", Remote = "", Local = "", BatchId = "";
        public bool IsPut;              // true = 上传（PUT），false = 下载（FETCH/FETCHDIR）
        public bool AtomicLocal;        // true = 先下到 <Local>.rfs-part，完成后再原子改名（票据路径用）
        public long Done, Total;
        public int State = (int)FetchJobState.Running;
        public string Message = "";
        public readonly CancellationTokenSource Cts = new();
    }

    private readonly System.Collections.Concurrent.ConcurrentDictionary<string, FetchJob> _fetchJobs = new();

    /// <summary>登记一个后台下载并立即返回（调用方把 job.Id 回给扩展）。</summary>
    private FetchJob StartFetchJob(string site, string remote, string local, string batchId, bool isDir, bool isPut = false)
    {
        var job = new FetchJob
        {
            Id = Guid.NewGuid().ToString("N"),
            Site = site, Remote = remote, Local = local, BatchId = batchId, IsPut = isPut,
        };
        _fetchJobs[job.Id] = job;
        Log($"fetch queued id='{job.Id}' site='{site}' remote='{remote}' temp='{local}' batch='{batchId}' kind={(isPut ? "put" : isDir ? "dir" : "file")} free={AvailableBytes(local)}");
        _ = Task.Run(async () =>
        {
            try
            {
                FetchResult result = isPut
                    ? await PutAsync(job)
                    : (isDir ? await FetchDirAsync(job) : await FetchAsync(job));
                job.Message = result.Message;
                Volatile.Write(ref job.State, (int)result.State);
                Log($"fetch terminal id='{job.Id}' batch='{job.BatchId}' state={result.State} message='{result.Message}'");
            }
            catch (Exception ex)
            {
                job.Message = SanitizeBridgeError(ex.Message);
                Volatile.Write(ref job.State, (int)FetchJobState.Failed);
                Log($"fetch unhandled id='{job.Id}' batch='{job.BatchId}' exception='{SanitizeBridgeError(ex.ToString())}'");
            }
            finally
            {
                // 任务留在字典里一会儿，让扩展来得及查到最终状态；之后由下次清理回收。
                _ = Task.Delay(TimeSpan.FromMinutes(5)).ContinueWith(t => _fetchJobs.TryRemove(job.Id, out var _));
            }
        });
        return job;
    }

    private int CancelFetchJobs(string batchId)
    {
        if (string.IsNullOrWhiteSpace(batchId)) return 0;
        int count = 0;
        foreach (FetchJob job in _fetchJobs.Values)
        {
            if (!string.Equals(job.BatchId, batchId, StringComparison.Ordinal) ||
                (FetchJobState)Volatile.Read(ref job.State) != FetchJobState.Running) continue;
            try
            {
                job.Cts.Cancel();
                count++;
                Log($"cancel requested id='{job.Id}' batch='{batchId}' temp='{job.Local}'");
            }
            catch (ObjectDisposedException) { }
        }
        return count;
    }

    private static bool DeleteIncompleteFetchOutput(string path)
    {
        try
        {
            if (!File.Exists(path)) return true;
            File.Delete(path);
            return !File.Exists(path);
        }
        catch { return false; }
    }
    private static long AvailableBytes(string path)
    {
        try
        {
            string? root = Path.GetPathRoot(Path.GetFullPath(path));
            return string.IsNullOrEmpty(root) ? -1 : new DriveInfo(root).AvailableFreeSpace;
        }
        catch { return -1; }
    }
    /// <summary>诊断日志（%TEMP%/rfs-tasks.log，与 TransferTaskService / SshConfigReader 同一份）。</summary>
    private static void Log(string message)
    {
        try
        {
            File.AppendAllText(Path.Combine(Path.GetTempPath(), "rfs-tasks.log"),
                DateTime.Now.ToString("HH:mm:ss.fff") + " " + message + Environment.NewLine);
        }
        catch { }
    }

    /// <summary>下载一个远程文件到本地路径。
    ///
    /// 归属：**传输队列**（上传/下载属于传输；删除/改权限/改所有者才属于操作队列）。
    /// `BeginManagedTask` 正是为"不由 CLI 进程执行的传输"准备的：它的取消回调直接接
    /// 我们的 CancellationToken，所以取消能真正中断一个正在下载的大文件。</summary>
    private async Task<FetchResult> FetchAsync(FetchJob job)
    {
        var siteName = job.Site; var remotePath = job.Remote; var localPath = job.Local; var batchId = job.BatchId;
        // 票据路径：先下到 <目标>.rfs-part，完成后再原子改名（半成品不落成正式名）。
        string fetchPath = job.AtomicLocal ? localPath + ".rfs-part" : localPath;
        using var cancellation = CancellationTokenSource.CreateLinkedTokenSource(_stop.Token, job.Cts.Token);
        TransferTask? task = null;
        try
        {
            cancellation.Token.ThrowIfCancellationRequested();
            var connection = FindConnection(siteName);
            var remote = NormalizeRemotePath(remotePath);
            if (string.IsNullOrWhiteSpace(localPath)) return new(FetchJobState.Failed, "missing local path");
            var fileName = System.IO.Path.GetFileName(localPath);
            // Cancel runs inline on the UI thread when the user clicks 取消; hand the
            // actual token cancel to the thread pool so SSH.NET teardown never blocks
            // the UI thread, and never let a throwing registration escape.
            task = _transfers?.BeginManagedTask("download", siteName, fileName, remote,
                () => System.Threading.ThreadPool.QueueUserWorkItem(_ => { try { cancellation.Cancel(); } catch { } }), batchId);
            using var fs = ProviderFactory.Create(connection);
            await Task.Run(() =>
            {
                cancellation.Token.ThrowIfCancellationRequested();
                fs.EnsureConnected();
                cancellation.Token.ThrowIfCancellationRequested();
                fs.Download(remote, fetchPath, (done, total) =>
                {
                    job.Done = done; job.Total = total;
                    if (task is not null)
                    {
                        _transfers?.WaitWhilePaused(task, cancellation.Token);
                        _transfers?.UpdateManagedTask(task, done, total, fileName);
                    }
                }, false, cancellation.Token);
            }, cancellation.Token);
            // A CANCEL racing the final progress callback still wins over success.
            cancellation.Token.ThrowIfCancellationRequested();
            if (job.AtomicLocal)
            {
                try { File.Move(fetchPath, localPath, overwrite: true); }
                catch (Exception ex)
                {
                    try { File.Delete(fetchPath); } catch { }
                    throw new IOException($"无法把 {fetchPath} 改名成 {localPath}: {ex.Message}", ex);
                }
            }
            if (task is not null) _transfers?.CompleteManagedTask(task, true, null);
            Log($"fetch done id='{job.Id}' site='{siteName}' remote='{remote}' temp='{localPath}' bytes={job.Total} free={AvailableBytes(localPath)} batch='{batchId}'");
            return new(FetchJobState.Done, "");
        }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
        {
            if (task is not null) _transfers?.CompleteManagedTask(task, false, null, cancelled: true);
            bool cleaned = DeleteIncompleteFetchOutput(fetchPath);
            Log($"fetch cancelled id='{job.Id}' site='{siteName}' remote='{remotePath}' temp='{localPath}' cleaned={cleaned} bytes={job.Done}/{job.Total} free={AvailableBytes(localPath)} batch='{batchId}'");
            return new(FetchJobState.Cancelled, "");
        }
        catch (Exception ex)
        {
            string message = SanitizeBridgeError(ex.Message);
            if (task is not null) _transfers?.CompleteManagedTask(task, false, message);
            bool cleaned = DeleteIncompleteFetchOutput(fetchPath);
            Log($"fetch failed id='{job.Id}' site='{siteName}' remote='{remotePath}' temp='{localPath}' cleaned={cleaned} bytes={job.Done}/{job.Total} free={AvailableBytes(localPath)} batch='{batchId}' exception='{SanitizeBridgeError(ex.ToString())}'");
            return new(FetchJobState.Failed, message);
        }
    }

    /// <summary>直传流：把远程文件**直接**写进调用方的流（命名管道），不落本地临时文件。
    /// 暂停/取消都作用在这条流上；调用方关掉管道即视为取消。</summary>
    private async Task StreamFetchAsync(FetchJob job, Stream output)
    {
        var siteName = job.Site; var remotePath = job.Remote; var batchId = job.BatchId;
        using var cancellation = CancellationTokenSource.CreateLinkedTokenSource(_stop.Token, job.Cts.Token);
        TransferTask? task = null;
        try
        {
            cancellation.Token.ThrowIfCancellationRequested();
            var connection = FindConnection(siteName);
            var remote = NormalizeRemotePath(remotePath);
            var fileName = System.IO.Path.GetFileName(remote);
            // 复制对话框驱动的直传流：**不允许暂停**（暂停会让 Shell 的复制停滞、Explorer
            // 显示忙碌光标），只能等待或取消 —— 见 TransferTask.CanPause。
            task = _transfers?.BeginManagedTask("download", siteName, fileName, remote,
                () => System.Threading.ThreadPool.QueueUserWorkItem(_ => { try { cancellation.Cancel(); } catch { } }),
                batchId, canPause: false);
            using var fs = ProviderFactory.Create(connection);
            await Task.Run(() =>
            {
                cancellation.Token.ThrowIfCancellationRequested();
                fs.EnsureConnected();
                cancellation.Token.ThrowIfCancellationRequested();
                fs.DownloadToStream(remote, output, (done, total) =>
                {
                    job.Done = done; job.Total = total;
                    if (task is not null) _transfers?.UpdateManagedTask(task, done, total, fileName);
                }, cancellation.Token);
            }, cancellation.Token);
            Volatile.Write(ref job.State, (int)FetchJobState.Done);
            if (task is not null) _transfers?.CompleteManagedTask(task, true, null);
            Log($"stream done id='{job.Id}' site='{siteName}' remote='{remote}' bytes={job.Done} batch='{batchId}'");
        }
        catch (Exception ex)
        {
            // 调用方关掉管道（用户取消/放弃）也是取消：IOException / 断管 / OCE。
            bool cancelled = cancellation.IsCancellationRequested || ex is IOException ||
                             ex.InnerException is IOException || ex is OperationCanceledException;
            if (cancelled)
            {
                Volatile.Write(ref job.State, (int)FetchJobState.Cancelled);
                if (task is not null) _transfers?.CompleteManagedTask(task, false, null, cancelled: true);
                Log($"stream cancelled id='{job.Id}' site='{siteName}' remote='{remotePath}' bytes={job.Done} batch='{batchId}'");
            }
            else
            {
                string message = SanitizeBridgeError(ex.Message);
                job.Message = message;
                Volatile.Write(ref job.State, (int)FetchJobState.Failed);
                if (task is not null) _transfers?.CompleteManagedTask(task, false, message);
                Log($"stream failed id='{job.Id}' site='{siteName}' remote='{remotePath}' bytes={job.Done} exception='{SanitizeBridgeError(ex.ToString())}'");
            }
        }
    }

    /// <summary>上传一个本地文件到远程（粘贴 / 编辑回写 / 跨站点复制）。
    /// 与 FetchAsync 同构：走**传输队列**、进度聚合到同一个任务上、取消是终态。</summary>
    private async Task<FetchResult> PutAsync(FetchJob job)
    {
        var siteName = job.Site; var remotePath = job.Remote; var localPath = job.Local; var batchId = job.BatchId;
        using var cancellation = CancellationTokenSource.CreateLinkedTokenSource(_stop.Token, job.Cts.Token);
        TransferTask? task = null;
        try
        {
            cancellation.Token.ThrowIfCancellationRequested();
            var connection = FindConnection(siteName);
            var remote = NormalizeRemotePath(remotePath);
            if (string.IsNullOrWhiteSpace(localPath))
                return new(FetchJobState.Failed, "missing local path");
            // 2026-09-25：拖放 / Ctrl+V 进来的**文件夹**此前被原样当文件上传，服务侧
            // File.Exists(目录) 恒为 false → 一律 "missing local file"，用户只看到
            // 「部分文件上传失败」，真因被盖住（中文路径是误会）。目录现在走递归上传。
            if (Directory.Exists(localPath))
                return await PutDirAsync(job);
            if (!File.Exists(localPath))
                return new(FetchJobState.Failed, $"local path not found: {localPath}");
            var fileName = System.IO.Path.GetFileName(localPath);
            task = _transfers?.BeginManagedTask("upload", siteName, fileName, remote,
                () => System.Threading.ThreadPool.QueueUserWorkItem(_ => { try { cancellation.Cancel(); } catch { } }), batchId);
            using var fs = ProviderFactory.Create(connection);
            await Task.Run(() =>
            {
                cancellation.Token.ThrowIfCancellationRequested();
                fs.EnsureConnected();
                cancellation.Token.ThrowIfCancellationRequested();
                fs.Upload(localPath, remote, (done, total) =>
                {
                    job.Done = done; job.Total = total;
                    if (task is not null) _transfers?.UpdateManagedTask(task, done, total, fileName);
                }, false, cancellation.Token,
                // 暂停闸门：SFTP 把它挂在**输入流**上（上传线程），不会卡住会话消息线程；
                // FTP 在传输线程的进度回调里调用。取消仍走 token。
                waitWhilePaused: task is null ? null : () => _transfers?.WaitWhilePaused(task, cancellation.Token));
            }, cancellation.Token);
            cancellation.Token.ThrowIfCancellationRequested();
            if (task is not null) _transfers?.CompleteManagedTask(task, true, null);
            Log($"put done id='{job.Id}' site='{siteName}' remote='{remote}' src='{localPath}' bytes={job.Total} batch='{batchId}'");
            return new(FetchJobState.Done, "");
        }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
        {
            if (task is not null) _transfers?.CompleteManagedTask(task, false, null, cancelled: true);
            Log($"put cancelled id='{job.Id}' site='{siteName}' remote='{remotePath}' src='{localPath}' bytes={job.Done}/{job.Total} batch='{batchId}'");
            return new(FetchJobState.Cancelled, "");
        }
        catch (Exception ex)
        {
            string message = SanitizeBridgeError(ex.Message);
            if (task is not null) _transfers?.CompleteManagedTask(task, false, message);
            Log($"put failed id='{job.Id}' site='{siteName}' remote='{remotePath}' src='{localPath}' bytes={job.Done}/{job.Total} exception='{SanitizeBridgeError(ex.ToString())}'");
            return new(FetchJobState.Failed, message);
        }
    }

    /// <summary>递归上传一个本地目录到远程（拖放 / Ctrl+V 里"文件夹"的那一项）。
    /// 与 FetchDirAsync 完全同构：先扫本地树拿文件总数，再建远程目录、逐个上传，
    /// 进度按**文件数**聚合到一个队列任务上，暂停/取消语义一致。
    /// 远程根 = remote（顶层文件夹名由扩展侧拼好，与下载侧的本地根对称）。</summary>
    private async Task<FetchResult> PutDirAsync(FetchJob job)
    {
        var siteName = job.Site; var remoteDir = job.Remote; var localRoot = job.Local; var batchId = job.BatchId;
        using var cancellation = CancellationTokenSource.CreateLinkedTokenSource(_stop.Token, job.Cts.Token);
        TransferTask? task = null;
        int uploaded = 0, failed = 0;
        try
        {
            cancellation.Token.ThrowIfCancellationRequested();
            var connection = FindConnection(siteName);
            var remote = NormalizeRemotePath(remoteDir);
            var folderName = System.IO.Path.GetFileName(localRoot.TrimEnd('\\', '/'));
            if (string.IsNullOrEmpty(folderName)) folderName = remote;
            task = _transfers?.BeginManagedTask("upload", siteName, folderName, remote,
                () => System.Threading.ThreadPool.QueueUserWorkItem(_ => { try { cancellation.Cancel(); } catch { } }), batchId);
            _transfers?.UpdateManagedTask(task!, 0, 0, Ui.IsEnglish ? "Scanning local folder" : "正在扫描本地目录");
            using var fs = ProviderFactory.Create(connection);
            await Task.Run(() =>
            {
                cancellation.Token.ThrowIfCancellationRequested();
                fs.EnsureConnected();
                cancellation.Token.ThrowIfCancellationRequested();
                var dirs = new List<string>();
                var files = new List<(string Local, string Remote)>();
                CollectLocalFiles(localRoot, remote, dirs, files, cancellation.Token);
                int total = files.Count;
                job.Done = 0; job.Total = total;
                if (task is not null)
                    _transfers?.UpdateManagedTask(task, 0, total, Ui.IsEnglish ? $"0 / {total} files" : $"0 / {total} 个文件");
                // 目录先建（浅→深）。CreateDirectory 对**已存在**的目标会报错（SFTP mkdir），
                // 这里刻意吞掉：目标目录本来就可能已经存在，真问题留给随后的上传去暴露。
                foreach (var dir in dirs)
                {
                    cancellation.Token.ThrowIfCancellationRequested();
                    try { fs.CreateDirectory(dir); } catch { }
                }
                foreach (var file in files)
                {
                    cancellation.Token.ThrowIfCancellationRequested();
                    _transfers?.WaitWhilePaused(task, cancellation.Token);
                    try
                    {
                        fs.Upload(file.Local, file.Remote, null, false, cancellation.Token,
                            task is null ? null : () => _transfers?.WaitWhilePaused(task, cancellation.Token));
                        uploaded++;
                    }
                    catch (OperationCanceledException) { throw; }
                    catch (Exception ex)
                    {
                        failed++;
                        Log($"putdir item failed id='{job.Id}' remote='{file.Remote}' src='{file.Local}' exception='{SanitizeBridgeError(ex.ToString())}'");
                    }
                    job.Done = uploaded + failed;
                    if (task is not null)
                        _transfers?.UpdateManagedTask(task, uploaded + failed, total,
                            Ui.IsEnglish ? $"{uploaded} / {total} files" : $"{uploaded} / {total} 个文件");
                }
            }, cancellation.Token);
            cancellation.Token.ThrowIfCancellationRequested();
            if (task is not null) _transfers?.CompleteManagedTask(task, failed == 0, failed == 0 ? null : $"{failed} file(s) failed");
            string message = failed == 0 ? "" : $"{failed} file(s) failed";
            Log($"putdir done id='{job.Id}' site='{siteName}' remote='{remote}' src='{localRoot}' files={uploaded} failed={failed} batch='{batchId}'");
            return new(failed == 0 ? FetchJobState.Done : FetchJobState.Failed, message);
        }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
        {
            if (task is not null) _transfers?.CompleteManagedTask(task, false, null, cancelled: true);
            Log($"putdir cancelled id='{job.Id}' site='{siteName}' remote='{remoteDir}' src='{localRoot}' files={uploaded}/{job.Total} batch='{batchId}'");
            return new(FetchJobState.Cancelled, "");
        }
        catch (Exception ex)
        {
            string message = SanitizeBridgeError(ex.Message);
            if (task is not null) _transfers?.CompleteManagedTask(task, false, message);
            Log($"putdir failed id='{job.Id}' site='{siteName}' remote='{remoteDir}' src='{localRoot}' files={uploaded}/{job.Total} batch='{batchId}' exception='{SanitizeBridgeError(ex.ToString())}'");
            return new(FetchJobState.Failed, message);
        }
    }

    /// <summary>把本地目录树摊平成 (本地, 远程) 文件列表，并收集需要创建的远程目录（浅→深）。
    /// 符号链接目录当**空目录**占位、不跟随目标 —— 与下载侧 CollectRemoteFiles 同一规则，
    /// 免得往服务器上灌一份链接目标的副本。</summary>
    private static void CollectLocalFiles(string localDir, string remoteDir,
                                          List<string> outDirs, List<(string Local, string Remote)> outFiles,
                                          CancellationToken token)
    {
        token.ThrowIfCancellationRequested();
        outDirs.Add(remoteDir);
        foreach (var entry in System.IO.Directory.EnumerateFileSystemEntries(localDir))
        {
            var name = System.IO.Path.GetFileName(entry);
            if (string.IsNullOrEmpty(name)) continue;
            var childRemote = remoteDir.TrimEnd('/') + "/" + name;
            var attrs = System.IO.File.GetAttributes(entry);
            if ((attrs & FileAttributes.Directory) != 0)
            {
                if ((attrs & FileAttributes.ReparsePoint) != 0) outDirs.Add(childRemote);
                else CollectLocalFiles(entry, childRemote, outDirs, outFiles, token);
            }
            else outFiles.Add((entry, childRemote));
        }
    }

    /// <summary>递归下载一个远程目录到本地根：**一个文件夹 = 一个队列任务**（用户要求），
    /// 同时拖多个文件夹时每个文件夹各自一组。
    ///
    /// 与 FetchAsync 一样进**传输队列**；先扫一遍树拿到文件总数，再逐个下载，
    /// 进度聚合到同一个任务上（与 DELETE 的"先列树再执行"同一套路）。</summary>
    private async Task<FetchResult> FetchDirAsync(FetchJob job)
    {
        var siteName = job.Site; var remoteDir = job.Remote; var localRoot = job.Local; var batchId = job.BatchId;
        using var cancellation = CancellationTokenSource.CreateLinkedTokenSource(_stop.Token, job.Cts.Token);
        TransferTask? task = null;
        int downloaded = 0, failed = 0;
        try
        {
            cancellation.Token.ThrowIfCancellationRequested();
            var connection = FindConnection(siteName);
            var remote = NormalizeRemotePath(remoteDir);
            if (string.IsNullOrWhiteSpace(localRoot)) return new(FetchJobState.Failed, "missing local root");
            var folderName = System.IO.Path.GetFileName(remote.TrimEnd('/'));
            if (string.IsNullOrEmpty(folderName)) folderName = remote;
            // 本地树**必须带顶层文件夹名**：Shell 扩展的 descriptor relPath 是
            // "small-5\\f-7723.txt"，LocalPath(rel) = <localRoot>\<rel>。旧实现
            // （CLI getr）也是这么落盘的（CmdGetR 里有明确注释）；搬到服务后一度丢掉
            // 这一层，导致 GetData(FILECONTENTS) 永远找不到文件（"fetch wait failed"）。
            var downloadRoot = System.IO.Path.Combine(localRoot, folderName);
            task = _transfers?.BeginManagedTask("download", siteName, folderName, remote,
                () => System.Threading.ThreadPool.QueueUserWorkItem(_ => { try { cancellation.Cancel(); } catch { } }), batchId);
            _transfers?.UpdateManagedTask(task!, 0, 0, Ui.IsEnglish ? "Scanning remote folder" : "正在扫描远程目录");
            using var fs = ProviderFactory.Create(connection);
            await Task.Run(() =>
            {
                cancellation.Token.ThrowIfCancellationRequested();
                fs.EnsureConnected();
                cancellation.Token.ThrowIfCancellationRequested();
                var files = new List<(string Remote, string Local)>();
                CollectRemoteFiles(fs, remote, downloadRoot, files, cancellation.Token);
                int total = files.Count;
                job.Done = 0; job.Total = total;
                if (task is not null) _transfers?.UpdateManagedTask(task, 0, total,
                    Ui.IsEnglish ? $"0 / {total} files" : $"0 / {total} 个文件");
                foreach (var file in files)
                {
                    cancellation.Token.ThrowIfCancellationRequested();
                    _transfers?.WaitWhilePaused(task, cancellation.Token);   // pause between files too
                    try
                    {
                        var dir = System.IO.Path.GetDirectoryName(file.Local);
                        if (!string.IsNullOrEmpty(dir)) System.IO.Directory.CreateDirectory(dir);
                        fs.Download(file.Remote, file.Local,
                            (_, _) => _transfers?.WaitWhilePaused(task, cancellation.Token), false, cancellation.Token);
                        downloaded++;
                    }
                    catch (OperationCanceledException) { throw; }
                    catch (Exception ex)
                    {
                        failed++;
                        Log($"fetchdir item failed id='{job.Id}' remote='{file.Remote}' exception='{SanitizeBridgeError(ex.ToString())}'");
                    }
                    job.Done = downloaded + failed;
                    if (task is not null) _transfers?.UpdateManagedTask(task, downloaded + failed, total,
                        Ui.IsEnglish ? $"{downloaded} / {total} files" : $"{downloaded} / {total} 个文件");
                }
            }, cancellation.Token);
            cancellation.Token.ThrowIfCancellationRequested();
            if (task is not null) _transfers?.CompleteManagedTask(task, failed == 0, failed == 0 ? null : $"{failed} file(s) failed");
            string message = failed == 0 ? "" : $"{failed} file(s) failed";
            Log($"fetchdir done id='{job.Id}' site='{siteName}' remote='{remote}' temp='{localRoot}' files={downloaded} failed={failed} free={AvailableBytes(localRoot)} batch='{batchId}'");
            return new(failed == 0 ? FetchJobState.Done : FetchJobState.Failed, message);
        }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
        {
            if (task is not null) _transfers?.CompleteManagedTask(task, false, null, cancelled: true);
            Log($"fetchdir cancelled id='{job.Id}' site='{siteName}' remote='{remoteDir}' temp='{localRoot}' files={downloaded}/{job.Total} free={AvailableBytes(localRoot)} batch='{batchId}'");
            return new(FetchJobState.Cancelled, "");
        }
        catch (Exception ex)
        {
            string message = SanitizeBridgeError(ex.Message);
            if (task is not null) _transfers?.CompleteManagedTask(task, false, message);
            Log($"fetchdir failed id='{job.Id}' site='{siteName}' remote='{remoteDir}' temp='{localRoot}' files={downloaded}/{job.Total} free={AvailableBytes(localRoot)} batch='{batchId}' exception='{SanitizeBridgeError(ex.ToString())}'");
            return new(FetchJobState.Failed, message);
        }
    }
    /// <summary>把远程目录树摊平成 (远程, 本地) 文件列表；本地目录顺手建出来。
    /// 符号链接当叶项目处理（不跟随目标），与 DELETE 的规则一致。</summary>
    private static void CollectRemoteFiles(IRemoteFileSystem fs, string remoteDir, string localDir,
                                           List<(string Remote, string Local)> outFiles, CancellationToken token)
    {
        token.ThrowIfCancellationRequested();
        System.IO.Directory.CreateDirectory(localDir);
        foreach (var entry in fs.List(remoteDir))
        {
            if (entry.Name is "." or "..") continue;
            var childRemote = remoteDir.TrimEnd('/') + "/" + entry.Name;
            var childLocal = System.IO.Path.Combine(localDir, entry.Name);
            if (entry.IsDirectory && !entry.IsSymlink)
                CollectRemoteFiles(fs, childRemote, childLocal, outFiles, token);
            else if (entry.IsDirectory)
                System.IO.Directory.CreateDirectory(childLocal);   // 符号链接目录：建个空目录占位
            else
                outFiles.Add((childRemote, childLocal));
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
                () => { try { cancellation.Cancel(); } catch { } });
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
        // 只有**我们自己的** token 被取消才算用户取消：SSH.NET 在超时/断连时
        // 也会抛 OperationCanceledException，那属于可重试的失败，不能回成 cancelled
        //（扩展侧对 cancelled 的语义是永久放弃、不再重试）。
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
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

    private static string SanitizeBridgeError(string message)
    {
        string clean = (message ?? "remote operation failed").Replace('\r', ' ').Replace('\n', ' ');
        // Diagnostic logs retain the exception chain but must never persist a
        // credential if a provider includes one in a connection-string error.
        return System.Text.RegularExpressions.Regex.Replace(
            clean, "(?i)\\b(password|pwd|passphrase)\\s*=\\s*[^\\s;,'\\\"]+", "$1=***");
    }

    private sealed record DeletePlanItem(string Path, bool IsDirectory);

    // ── 导航加速：本地持久缓存先行（2026-09-24）──────────────────────────────
    // 之前 erf: 直达 = 服务端逐段真实 LIST（内存列表缓存只有 3 秒，等于每次重走网络），
    // 深路径要串行等 N 次往返。现在同一条 erf-cache.db 里加一张服务侧的
    // bridge_listings(site, path, tick, text)：text 就是 ITEM 行文本（无二进制耦合），
    // 并顺带查 DLL 写的 dir_cache —— 两边任一命中该段就不再打网络。
    // 结果：走过的路径再直达 = 0 次网络往返，和本地目录一样秒开；
    // 没走过的路径行为不变（逐段 LIST，且这次会把每段记进 bridge_listings）。
    private string? _cacheDbPath;
    private static int _bridgeListingWrites;

    private static string? ResolveCacheDbPath()
    {
        try
        {
            var dir = AppSettings.Load().MetadataCachePath;
            if (string.IsNullOrWhiteSpace(dir)) dir = AppSettings.DefaultMetadataCachePath;
            return Path.Combine(dir, "erf-cache.db");
        }
        catch { return null; }
    }

    private SqliteConnection? OpenCacheDb()
    {
        try
        {
            _cacheDbPath ??= ResolveCacheDbPath();
            if (_cacheDbPath is null) return null;
            var connection = new SqliteConnection(new SqliteConnectionStringBuilder
            {
                DataSource = _cacheDbPath,
                Mode = SqliteOpenMode.ReadWriteCreate,
                DefaultTimeout = 4,
            }.ToString());
            connection.Open();
            using var pragma = connection.CreateCommand();
            pragma.CommandText = "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA busy_timeout=4000;";
            pragma.ExecuteNonQuery();
            using var create = connection.CreateCommand();
            create.CommandText =
                "CREATE TABLE IF NOT EXISTS bridge_listings(" +
                "  site TEXT NOT NULL, path TEXT NOT NULL, tick INTEGER NOT NULL," +
                "  text TEXT NOT NULL, PRIMARY KEY(site, path));";
            create.ExecuteNonQuery();
            return connection;
        }
        catch { return null; }
    }

    private static TimeSpan BridgeListingMaxAge()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(@"Software\ExplorerRemoteFs");
            var hours = Convert.ToInt32(key?.GetValue("DirCacheDiskMaxAgeHours") ?? 24);
            if (hours < 1) hours = 1;
            if (hours > 720) hours = 720;
            return TimeSpan.FromHours(hours);
        }
        catch { return TimeSpan.FromHours(24); }
    }

    // DLL 写的 dir_cache 里有没有这个 (site, path) 的新鲜行 —— 行存在 = 之前真实列过该目录。
    private static bool HasFreshDirCacheRow(SqliteConnection db, string site, string path, TimeSpan maxAge)
    {
        try
        {
            using var cmd = db.CreateCommand();
            cmd.CommandText = "SELECT tick FROM dir_cache WHERE site=$s AND path=$p";
            cmd.Parameters.AddWithValue("$s", site);
            cmd.Parameters.AddWithValue("$p", path);
            var value = cmd.ExecuteScalar();
            if (value is null || value is DBNull) return false;
            var ageMs = (DateTime.UtcNow.Ticks - Convert.ToInt64(value)) / TimeSpan.TicksPerMillisecond;
            return ageMs >= 0 && ageMs <= maxAge.TotalMilliseconds;
        }
        catch { return false; }
    }

    private static string? TryGetBridgeListing(SqliteConnection db, string site, string path, TimeSpan maxAge)
    {
        try
        {
            using var cmd = db.CreateCommand();
            cmd.CommandText = "SELECT tick, text FROM bridge_listings WHERE site=$s AND path=$p";
            cmd.Parameters.AddWithValue("$s", site);
            cmd.Parameters.AddWithValue("$p", path);
            using var reader = cmd.ExecuteReader();
            if (!reader.Read()) return null;
            var ageMs = (DateTime.UtcNow.Ticks - reader.GetInt64(0)) / TimeSpan.TicksPerMillisecond;
            if (ageMs < 0 || ageMs > maxAge.TotalMilliseconds) return null;
            return reader.GetString(1);
        }
        catch { return null; }
    }

    private void StoreBridgeListing(SqliteConnection? db, string site, string path, string text)
    {
        if (db is null) return;
        try
        {
            using var cmd = db.CreateCommand();
            cmd.CommandText =
                "INSERT INTO bridge_listings(site, path, tick, text) VALUES($s, $p, $t, $x) " +
                "ON CONFLICT(site, path) DO UPDATE SET tick=excluded.tick, text=excluded.text";
            cmd.Parameters.AddWithValue("$s", site);
            cmd.Parameters.AddWithValue("$p", path);
            cmd.Parameters.AddWithValue("$t", DateTime.UtcNow.Ticks);
            cmd.Parameters.AddWithValue("$x", text);
            cmd.ExecuteNonQuery();
            // 轻量清理：每 64 次写入删一次 7 天前的旧行，免得无限长。
            if (System.Threading.Interlocked.Increment(ref _bridgeListingWrites) % 64 == 0)
            {
                using var prune = db.CreateCommand();
                prune.CommandText = "DELETE FROM bridge_listings WHERE tick < $cutoff";
                prune.Parameters.AddWithValue("$cutoff", DateTime.UtcNow.AddDays(-7).Ticks);
                prune.ExecuteNonQuery();
            }
        }
        catch { /* 缓存写失败绝不影响导航 */ }
    }

    private async Task WarmErfNavigationAsync(string address)
    {
        if (!TryParseErfAddress(address, out var siteName, out var requestedPath))
            throw new InvalidOperationException("Invalid ERF address. Expected erf:<site>:/absolute/unix/path.");

        var connection = FindConnection(siteName);
        var canonicalSite = connection.Name;
        var startPath = NormalizeRemotePath(connection.StartPath);
        var fullPath = NormalizeRemotePath(requestedPath);
        if (!IsAtOrBelow(fullPath, startPath))
            throw new InvalidOperationException($"The ERF path is outside the configured start path '{startPath}'.");

        var maxAge = BridgeListingMaxAge();
        using var db = OpenCacheDb();

        var currentPath = startPath;
        var remainder = fullPath.Length == startPath.Length ? string.Empty : fullPath[startPath.Length..].TrimStart('/');
        foreach (var segment in remainder.Split('/', StringSplitOptions.RemoveEmptyEntries))
        {
            var childPath = currentPath == "/" ? "/" + segment : currentPath + "/" + segment;
            // 该段已在本地缓存中（DLL 列过这个目录，或服务侧存过父目录的列表）→ 零网络。
            if (db is not null && HasFreshDirCacheRow(db, canonicalSite, childPath, maxAge))
            {
                currentPath = childPath;
                continue;
            }
            if (db is not null &&
                TryGetBridgeListing(db, canonicalSite, currentPath, maxAge) is { } cachedText &&
                ListingContainsDirectory(cachedText, segment))
            {
                currentPath = childPath;
                continue;
            }
            var listing = await GetListingAsync(canonicalSite, currentPath);
            if (listing.StartsWith("FAIL:", StringComparison.Ordinal))
                throw new InvalidOperationException(listing[5..].Trim());
            if (!ListingContainsDirectory(listing, segment))
                throw new DirectoryNotFoundException($"Remote directory not found: {fullPath}");
            currentPath = childPath;
        }

        // Prewarm the target view as well; the first Explorer enumeration can
        // consume this completed cache entry without another remote round trip.
        // DLL 自己已有该目录快照时连这次也省掉（它根本不会再问服务）。
        if (db is null || !HasFreshDirCacheRow(db, canonicalSite, currentPath, maxAge))
        {
            var targetListing = await GetListingAsync(canonicalSite, currentPath);
            if (targetListing.StartsWith("FAIL:", StringComparison.Ordinal))
                throw new InvalidOperationException(targetListing[5..].Trim());
        }
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
                var listingText = text.ToString();
                // 每次真实 LIST 成功后记进持久缓存：同一条 erf: 地址下次直达就不用再走网络。
                using (var db = OpenCacheDb())
                    StoreBridgeListing(db, connection.Name, remotePath, listingText);
                return listingText;
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
        // 大小写语义见 FindConnection(string, out string) 的注释：
        // 精确优先，唯一不敏感回退，歧义按找不到。
        var connection = FindConnection(name, out _);
        if (connection is null)
            throw new InvalidOperationException($"Connection '{name}' not found.");
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
