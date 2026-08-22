using System.Text;

namespace ExplorerRemoteFs.Utils;

/// <summary>
/// 文件日志（%APPDATA%\ExplorerRemoteFs\debug.log），用于排查 Shell 扩展问题。
/// </summary>
public static class ShellLog
{
    private static readonly object Gate = new();
    private static readonly string LogPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "ExplorerRemoteFs", "debug.log");

    public static void Write(string message)
    {
        try
        {
            lock (Gate)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(LogPath)!);
                File.AppendAllText(LogPath, $"[{DateTime.Now:HH:mm:ss.fff}] {message}{Environment.NewLine}", Encoding.UTF8);
            }
        }
        catch
        {
            // 日志失败不影响主流程
        }
    }
}
