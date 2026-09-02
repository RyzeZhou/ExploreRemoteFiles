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
    private readonly CancellationTokenSource _stop = new();
    private readonly SemaphoreSlim _providerGate = new(1, 1);
    private readonly object _listingCacheGate = new();
    private readonly Dictionary<string, CachedListing> _listingCache = new(StringComparer.Ordinal);
    private Task? _listener;

    private sealed record CachedListing(DateTimeOffset ExpiresAt, Task<string> Response);

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
            await writer.WriteAsync(response);
        }
        catch (Exception ex)
        {
            await writer.WriteLineAsync($"FAIL: {ex.Message.Replace('\r', ' ').Replace('\n', ' ')}");
            if (!string.IsNullOrWhiteSpace(siteName)) ProviderFactory.Invalidate(siteName);
        }
    }

    // Explorer can ask for the same uncached directory concurrently while it
    // binds the target and prepares the new view. Share one backend list call
    // and keep the completed text briefly, so those requests finish together.
    private Task<string> GetListingAsync(string siteName, string remotePath)
    {
        var key = siteName.ToUpperInvariant() + "\0" + remotePath;
        lock (_listingCacheGate)
        {
            var now = DateTimeOffset.UtcNow;
            if (_listingCache.TryGetValue(key, out var existing) && existing.ExpiresAt > now)
                return existing.Response;

            var task = BuildListingAsync(siteName, remotePath);
            _listingCache[key] = new CachedListing(now.AddSeconds(30), task);
            foreach (var expired in _listingCache.Where(p => p.Value.ExpiresAt <= now).Select(p => p.Key).ToArray())
                _listingCache.Remove(expired);
            return task;
        }
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
}