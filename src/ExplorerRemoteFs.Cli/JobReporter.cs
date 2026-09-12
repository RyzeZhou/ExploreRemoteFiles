using System.IO.Pipes;
using System.Text;

/// <summary>
/// Reports a transfer job (upload/download) to the resident service's task
/// window over a dedicated named pipe. Every failure is swallowed: progress
/// reporting is UI only and must never affect the transfer itself.
///
/// Wire format (UTF-8 lines, TAB separated):
///   B  id  direction(upload|download)  server  name  localPath  remotePath  totalBytes  pid
///   P  id  doneBytes  totalBytes  [currentFile]
///   E  id  status(done|fail)  message
/// </summary>
internal static class JobReporter
{
    private const string PipeName = "ExplorerRemoteFs.Tasks.v1";
    private static readonly object Gate = new();
    private static NamedPipeClientStream? _pipe;
    private static StreamWriter? _writer;
    /// <summary>Pause gate: signaled = running, reset = paused. The service
    /// resets/sets it by name (<see cref="GatePrefix"/> + job id + ".Gate") to
    /// pause/resume the transfer; waiting inside the progress callback blocks
    /// the transfer thread, which is exactly what a pause needs.</summary>
    private static EventWaitHandle? _gate;
    private const string GatePrefix = @"Local\ExplorerRemoteFs.Job.";
    private static string _id = "";
    private static long _lastSent;
    private static DateTime _lastAt = DateTime.MinValue;
    private static string _lastFile = "";

    public static void Begin(string direction, string server, string localPath, string remotePath, long total)
    {
        lock (Gate)
        {
            Close();
            _id = Guid.NewGuid().ToString("N")[..8];
            _lastSent = 0;
            _lastAt = DateTime.MinValue;
            _lastFile = "";
            string name = Path.GetFileName(remotePath.TrimEnd('/'));
            if (string.IsNullOrEmpty(name)) name = remotePath;
            try
            {
                _gate = new EventWaitHandle(true, EventResetMode.ManualReset, GatePrefix + _id + ".Gate");
            }
            catch { _gate = null; }
            Send($"B\t{_id}\t{direction}\t{server}\t{name}\t{localPath}\t{remotePath}\t{total}\t{Environment.ProcessId}");
        }
    }

    public static void Progress(long done, long total) => Progress(done, total, null);

    /// <summary>
    /// Progress of a job. For a MULTI-FILE job (a whole directory tree fetched
    /// as one transfer, mirroring WinSCP's queue where one entry represents a
    /// transfer and not a file) <paramref name="currentFile"/> names the file in
    /// flight; the service shows it on the batch's second line.
    /// bytes are cumulative across ALL files of the job.
    /// </summary>
    public static void Progress(long done, long total, string? currentFile)
    {
        // Paused? Block the transfer thread here (the callback runs on it), in
        // short slices so a resume is picked up promptly. Killing the process
        // (cancel) ends the wait immediately with the process.
        EventWaitHandle? gate = _gate;
        if (gate is not null)
        {
            while (!gate.WaitOne(200)) { /* paused */ }
        }
        lock (Gate)
        {
            if (_writer is null) return;
            DateTime now = DateTime.UtcNow;
            string file = currentFile ?? "";
            bool fileChanged = !string.Equals(file, _lastFile, StringComparison.Ordinal);
            // Throttle to ~8 updates/s (plus always report the final byte and
            // every file change) so a fast transfer cannot flood the window.
            if (!fileChanged && now - _lastAt < TimeSpan.FromMilliseconds(120) && done - _lastSent < 262144) return;
            _lastAt = now;
            _lastSent = done;
            _lastFile = file;
            Send($"P\t{_id}\t{done}\t{total}\t{file}");
        }
    }

    /// <summary>Appends a diagnostic line (resume decisions etc.) to
    /// %TEMP%/rfs-cli.log — handy when verifying that resume actually engaged.</summary>
    public static void Note(string message)
    {
        try
        {
            File.AppendAllText(Path.Combine(Path.GetTempPath(), "rfs-cli.log"),
                DateTime.Now.ToString("HH:mm:ss.fff") + " " + message + Environment.NewLine);
        }
        catch { }
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
        try { _gate?.Dispose(); } catch { }
        _writer = null;
        _pipe = null;
        _gate = null;
    }
}
