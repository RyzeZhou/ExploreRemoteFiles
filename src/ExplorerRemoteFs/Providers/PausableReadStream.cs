using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

namespace ExplorerRemoteFs.Providers;

/// <summary>
/// 把本地输入流包一层：每次读取前先调用 <c>waitWhilePaused()</c>（暂停闸门 + 取消检查）。
///
/// 为什么放在流上、而不是放在进度回调里：
/// SSH.NET 的**上传**进度回调跑在**会话消息线程**上
/// （<c>InternalUploadFile</c> 在 FXP_WRITE 的响应回调里 <c>uploadProgress.Report</c>），
/// 在那里阻塞会把整条连接卡死（连响应都收不到）；
/// 而上传循环读取输入流是在**上传任务自己的线程**上
/// （<c>await input.ReadAsync(...)</c>），在那里阻塞只暂停这次上传，安全。
/// </summary>
internal sealed class PausableReadStream : Stream
{
    private readonly Stream _inner;
    private readonly Action? _waitWhilePaused;

    public PausableReadStream(Stream inner, Action? waitWhilePaused)
    {
        _inner = inner;
        _waitWhilePaused = waitWhilePaused;
    }

    // 暂停闸门（若没有就是纯透传）。
    private void Gate() => _waitWhilePaused?.Invoke();

    public override bool CanRead => _inner.CanRead;
    public override bool CanSeek => _inner.CanSeek;
    public override bool CanWrite => false;
    public override long Length => _inner.Length;
    public override long Position { get => _inner.Position; set => _inner.Position = value; }

    public override int Read(byte[] buffer, int offset, int count)
    {
        Gate();
        return _inner.Read(buffer, offset, count);
    }

    public override Task<int> ReadAsync(byte[] buffer, int offset, int count, CancellationToken cancellationToken)
    {
        Gate();
        return _inner.ReadAsync(buffer, offset, count, cancellationToken);
    }

    public override ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default)
    {
        Gate();
        return _inner.ReadAsync(buffer, cancellationToken);
    }

    public override int ReadByte()
    {
        Gate();
        return _inner.ReadByte();
    }

    public override long Seek(long offset, SeekOrigin origin) => _inner.Seek(offset, origin);
    public override void SetLength(long value) => throw new NotSupportedException();
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
    public override void Flush() => _inner.Flush();

    protected override void Dispose(bool disposing)
    {
        if (disposing) _inner.Dispose();
        base.Dispose(disposing);
    }
}
