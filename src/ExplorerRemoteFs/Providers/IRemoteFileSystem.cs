namespace ExplorerRemoteFs.Providers;

/// <summary>
/// 递归权限修改的结果。<b>Failed &gt; 0 即"部分完成"，调用方不得报成功</b>
/// ——同一类错误（部分完成却报成功）在复制摊平上限上已经犯过一次。
/// </summary>
public sealed record ChmodRecursiveResult(int Dirs, int Files, IReadOnlyList<string> Failures)
{
    public bool Partial => Failures.Count > 0;
    public override string ToString() => $"dirs={Dirs} files={Files} failed={Failures.Count}";
}

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

    /// <summary>创建一个零字节文件；目标已存在时必须失败，不得覆盖。</summary>
    void CreateEmptyFile(string path);

    /// <summary>下载远程文件到本地路径。progress(已传字节, 总字节) 可选；resume 时从断点续传。</summary>
    void Download(string remotePath, string localPath, Action<long, long>? progress = null, bool resume = false);

    /// <summary>上传本地文件到远程路径。progress(已传字节, 总字节) 可选；resume 时从断点续传。</summary>
    void Upload(string localPath, string remotePath, Action<long, long>? progress = null, bool resume = false);

    /// <summary>修改远程权限（chmod；mode 为 8 进制数字，如 640）。</summary>
    void SetPermissions(string path, int mode);

    /// <summary>
    /// 递归修改目录树权限（chmod -R；mode 为 8 进制数字）。
    /// <b>目录与文件都会改</b>；符号链接<b>不跟随</b>（不通过链接改其目标权限）。
    /// 单个条目失败不中断整棵树，结果里带失败清单。
    /// <paramref name="onItem"/> 每处理一个条目回调一次（当前路径），供进度窗口显示；
    /// <paramref name="cancellation"/> 用于「取消」——取消后抛出 OperationCanceledException，
    /// 已改过的条目保持已改状态（不做回滚，与 chmod -R 的语义一致）。
    /// </summary>
    ChmodRecursiveResult SetPermissionsRecursive(string path, int mode,
        Action<string>? onItem = null, CancellationToken cancellation = default);

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
