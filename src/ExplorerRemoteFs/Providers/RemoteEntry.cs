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

    public long Size { get; init; }

    public DateTime? LastWriteTime { get; init; }

    public string? SymlinkTarget { get; init; }

    public string ModeDisplay => Mode ?? "";

    public string OwnerDisplay => Owner ?? "";

    public string GroupDisplay => Group ?? "";

    public string SizeDisplay => IsDirectory ? "" : FormatSize(Size);

    public string ModifiedDisplay => LastWriteTime?.ToString("yyyy-MM-dd HH:mm") ?? "";

    public static string FormatSize(long bytes)
    {
        if (bytes < 1024) return $"{bytes} B";
        string[] units = { "KB", "MB", "GB", "TB" };
        double v = bytes;
        var unit = "B";
        foreach (var u in units)
        {
            v /= 1024;
            unit = u;
            if (v < 1024) break;
        }
        return $"{v:0.#} {unit}";
    }
}
