using System.IO.Pipes;
using System.Text;

/// <summary>
/// Reports a transfer job (upload/download) to the resident service's task
/// window over a dedicated named pipe. Every failure is swallowed: progress
/// reporting is UI only and must never affect the transfer itself.
///
/// Wire format (UTF-8 lines, TAB separated):
///   B  id  direction(upload|download)  server  name  localPath  remotePath  totalBytes
///   P  id  doneBytes  totalBytes
///   E  id  status(done|fail)  message
/// </summary>
internal static class JobReporter
{
    private const string PipeName = "ExplorerRemoteFs.Tasks.v1";
    private static readonly object Gate = new();
    private static NamedPipeClientStream? _pipe;
    private static StreamWriter? _writer;
    private static string _id = "";
    private static long _lastSent;
    private static DateTime _lastAt = DateTime.MinValue;

    public static void Begin(string direction, string server, string localPath, string remotePath, long total)
    {
        lock (Gate)
        {
            Close();
            _id = Guid.NewGuid().ToString("N")[..8];
            _lastSent = 0;
            _lastAt = DateTime.MinValue;
            string name = Path.GetFileName(remotePath.TrimEnd('/'));
            if (string.IsNullOrEmpty(name)) name = remotePath;
            Send($"B\t{_id}\t{direction}\t{server}\t{name}\t{localPath}\t{remotePath}\t{total}");
        }
    }

    public static void Progress(long done, long total)
    {
        lock (Gate)
        {
            if (_writer is null) return;
            DateTime now = DateTime.UtcNow;
            // Throttle to ~8 updates/s (plus always report the final byte) so a
            // fast transfer cannot flood the pipe/window.
            if (now - _lastAt < TimeSpan.FromMilliseconds(120) && done - _lastSent < 262144) return;
            _lastAt = now;
            _lastSent = done;
            Send($"P\t{_id}\t{done}\t{total}");
        }
    }

    public static void End(bool ok, string message = "")
    {
        lock (Gate)
        {
            Send($"E\t{_id}\t{(ok ? "done" : "fail")}\t{message.Replace('\t', ' ')}");
            Close();
        }
    }

    private static void Send(string line)
    {
        try
        {
            if (_writer is null)
            {
                _pipe = new NamedPipeClientStream(".", PipeName, PipeDirection.Out);
                _pipe.Connect(400);       // service not running -> give up quietly
                _writer = new StreamWriter(_pipe, new UTF8Encoding(false)) { AutoFlush = true };
            }
            _writer.WriteLine(line);
        }
        catch { Close(); }
    }

    private static void Close()
    {
        try { _writer?.Dispose(); } catch { }
        try { _pipe?.Dispose(); } catch { }
        _writer = null;
        _pipe = null;
    }
}
