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

    /// <summary>删除文件或空目录（远程）。</summary>
    void Delete(string path);

    /// <summary>重命名/移动（远程）。</summary>
    void Rename(string from, string to);

    /// <summary>创建目录（远程）。</summary>
    void CreateDirectory(string path);

    /// <summary>下载远程文件到本地路径。progress(已传字节, 总字节) 可选。</summary>
    void Download(string remotePath, string localPath, Action<long, long>? progress = null);

    /// <summary>上传本地文件到远程路径。progress(已传字节, 总字节) 可选。</summary>
    void Upload(string localPath, string remotePath, Action<long, long>? progress = null);

    /// <summary>修改远程权限（chmod；mode 为 8 进制数字，如 640）。</summary>
    void SetPermissions(string path, int mode);

    /// <summary>递归修改目录树权限（chmod -R；mode 为 8 进制数字）。</summary>
    void SetPermissionsRecursive(string path, int mode);

    /// <summary>
    /// 修改所有者/组（chown/chgrp）。user/group 传 null 表示不修改；数字或名字均可（
    /// 实现层按协议能力处理：SFTP 支持，FTP 一般不支持）。
    /// </summary>
    void SetOwner(string path, string? user, string? group);

    /// <summary>服务器端复制（duplicate：远端到远端，源到目标）。</summary>
    void Copy(string from, string to);

    /// <summary>显示名（日志用）。</summary>
    string DisplayName { get; }
}
