namespace ExplorerRemoteFs.Providers;

/// <summary>
/// 统一的远程条目模型（对应探索计划 §4 P2：统一的是操作接口，不统一 metadata 语义）。
/// </summary>
public sealed class RemoteEntry
{
    public required string Name { get; init; }
    public required string Path { get; init; }
    public bool IsDirectory { get; init; }
    public bool IsSymlink { get; init; }
    public bool IsFile => !IsDirectory;

    /// <summary>POSIX 模式字符串，如 "drwxr-xr-x"；服务器不支持时为空。</summary>
    public string? Mode { get; init; }

    /// <summary>Owner（名字或数字 UID）。</summary>
    public string? Owner { get; init; }

    /// <summary>Group（名字或数字 GID）。</summary>
    public string? Group { get; init; }

    /// <summary>数字 UID；未知为 -1（FTP LIST 无数字 ID；SFTP 有）。</summary>
    public long Uid { get; init; } = -1;

    /// <summary>数字 GID；未知为 -1。</summary>
    public long Gid { get; init; } = -1;

    public long Size { get; init; }

    /// <summary>修改时间，**统一为 UTC 时刻（Kind=Utc）**：由各 Provider 规范化
    /// （SFTP 从本地字面值换算，FTP 按站点偏移从无时区字面值换算）。
    /// 显示口径由 <see cref="Utils.TimeDisplay"/> 决定，别在这里直接 ToString ——
    /// 之前就是在这里直出字面值，导致同一条链路里 CLI 与资源管理器显示的时区不一致。</summary>
    public DateTime? LastWriteTime { get; init; }

    /// <summary>服务器时区偏移（分钟，东八区 = 480）；null = 未探测到。
    /// FTP 的 LIST 需要它才能换算绝对时刻；SFTP 的 Unix 秒不需要，但「服务器时区」口径要用。</summary>
    public int? ServerUtcOffsetMinutes { get; init; }

    public string? SymlinkTarget { get; init; }

    public string ModeDisplay => Mode ?? "";

    /// <summary>所有者显示名：优先账户名（/etc/passwd 映射），取不到才退回数字 uid。</summary>
    public string OwnerDisplay => !string.IsNullOrEmpty(Owner) ? Owner : (Uid >= 0 ? Uid.ToString() : "");

    /// <summary>属组显示名：优先组名（/etc/group 映射），取不到才退回数字 gid。</summary>
    public string GroupDisplay => !string.IsNullOrEmpty(Group) ? Group : (Gid >= 0 ? Gid.ToString() : "");

    public string SizeDisplay => IsDirectory ? "" : FormatSize(Size);

    public string ModifiedDisplay => LastWriteTime is { } t
        ? Utils.TimeDisplay.Format(t, Utils.TimeDisplay.CurrentMode, ServerUtcOffsetMinutes)
        : "";

    /// 大小显示口径（1024/1000、KB/kB/KiB）在 Utils.SizeFormat 里统一实现，
    /// 与扩展 DLL 的 SizeFormat.h 用同一个注册表设置，避免两边各写一套。
    public static string FormatSize(long bytes, Utils.SizeFormatMode? mode = null)
        => Utils.SizeFormat.Format(bytes, mode ?? Utils.SizeFormat.CurrentMode);
}
