namespace ExplorerRemoteFs.Providers;

/// <summary>
/// 进度回调的防弹包装。
///
/// 为什么需要它：SSH.NET 的同步 <c>DownloadFile(path, stream, callback)</c> 会把回调包进
/// 内部的 <c>ThreadPoolProgress</c> 丢到线程池执行，并且写死 <c>CancellationToken.None</c>。
/// 于是回调里抛出的任何异常都变成"线程池未处理异常"，直接终止宿主进程 ——
/// 2026-09-20 实测：在传输队列点「取消」后 RemoteFsClient 进程以
/// <c>0xe0434352</c> / <c>System.OperationCanceledException</c> 崩溃，栈顶就是
/// <c>Renci.SshNet.SftpClient.ThreadPoolProgress</c>。而且那个回调根本取消不了下载，
/// 只会炸进程。
///
/// 进度上报只是 UI 副作用，绝不能影响传输本身、更不能终止宿主进程。真正的取消一律
/// 交给传输 API 自己的 <c>CancellationToken</c>（见 <see cref="SftpFileSystem.Download"/>
/// 改用异步重载）。
/// </summary>
internal sealed class SafeProgress<T> : IProgress<T>
{
    private readonly Action<T> _handler;

    public SafeProgress(Action<T> handler) => _handler = handler;

    public void Report(T value)
    {
        try { _handler(value); }
        catch { /* 见类型注释：进度回调永不抛出 */ }
    }
}
