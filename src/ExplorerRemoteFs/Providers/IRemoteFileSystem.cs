namespace ExplorerRemoteFs.Providers;

/// <summary>
/// 统一文件系统 Provider 接口（探索计划 §4 P2）。
/// 第一版只实现浏览所需的 List；Rename/Delete/SetMetadata 等随 Prototype 2+ 扩展。
/// </summary>
public interface IRemoteFileSystem : IDisposable
{
    /// <summary>确保已连接（连接池复用场景下可能已连接）。</summary>
    void EnsureConnected();

    /// <summary>列出目录内容（不含 . 和 ..）。</summary>
    IReadOnlyList<RemoteEntry> List(string path);

    /// <summary>显示名（日志用）。</summary>
    string DisplayName { get; }
}
