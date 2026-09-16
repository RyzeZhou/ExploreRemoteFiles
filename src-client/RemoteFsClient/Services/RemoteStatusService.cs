using System.Drawing;
using System.Windows.Interop;

namespace RemoteFsClient.Services;

/// <summary>托盘图标的两个状态位：<b>R</b> = 远程站点连接，<b>F</b> = 文件传输。
///
/// R（Remote）：只要有一个**正在使用**的站点最近一次操作失败，就是红；
/// 该站点下一次成功即恢复绿（所以修好网络、重新浏览一下就会变回来）。
/// F（Transfer）：没有传输=绿；有传输且无错=黄；出现过失败任务=红
/// （在传输窗口清掉已完成条目后恢复绿）。
///
/// 这个服务是**状态的唯一来源**：桥接（站点操作结果）与传输服务都往这里报，
/// 托盘图标只订阅它，避免各处各画一套。
/// </summary>
public sealed class RemoteStatusService
{
    private readonly object _gate = new();
    private readonly Dictionary<string, bool> _sites = new(StringComparer.OrdinalIgnoreCase);
    private ErfIcon.TransferState _transfer = ErfIcon.TransferState.Idle;

    /// <summary>状态变化（可能在任意线程触发，订阅方自行编组）。</summary>
    public event Action? Changed;

    public ErfIcon.RemoteState Remote
    {
        get { lock (_gate) return _sites.Values.Any(ok => !ok) ? ErfIcon.RemoteState.Problem : ErfIcon.RemoteState.Ok; }
    }

    public ErfIcon.TransferState Transfer
    {
        get { lock (_gate) return _transfer; }
    }

    /// <summary>报一次站点操作结果（LIST / 删除 / 改权限 / 改属主都算）。</summary>
    public void ReportSite(string? site, bool ok)
    {
        if (string.IsNullOrWhiteSpace(site)) return;
        bool changed;
        lock (_gate)
        {
            _sites.TryGetValue(site, out bool previous);
            bool had = _sites.ContainsKey(site);
            changed = !had || previous != ok;
            _sites[site] = ok;
        }
        if (changed) Changed?.Invoke();
    }

    /// <summary>站点被移除或连接作废时清掉它的记录（免得留一个永远红的站点）。</summary>
    public void ForgetSite(string? site)
    {
        if (string.IsNullOrWhiteSpace(site)) return;
        bool changed;
        lock (_gate) changed = _sites.Remove(site);
        if (changed) Changed?.Invoke();
    }

    public void SetTransfer(ErfIcon.TransferState state)
    {
        bool changed;
        lock (_gate)
        {
            changed = _transfer != state;
            _transfer = state;
        }
        if (changed) Changed?.Invoke();
    }

    /// <summary>当前状态对应的图标（缓存，见 ErfIcon.ToIcon）。</summary>
    public Icon TrayIcon(int size = 16) => ErfIcon.ToIcon(size, ErfIcon.ColorFor(Remote), ErfIcon.ColorFor(Transfer));

    /// <summary>给 WPF 窗口标题栏用（HICON → BitmapSource）。</summary>
    public System.Windows.Media.ImageSource WindowIcon(int size = 32)
    {
        using var icon = ErfIcon.ToIcon(size, ErfIcon.ColorFor(Remote), ErfIcon.ColorFor(Transfer));
        var source = Imaging.CreateBitmapSourceFromHIcon(icon.Handle,
            System.Windows.Int32Rect.Empty,
            System.Windows.Media.Imaging.BitmapSizeOptions.FromEmptyOptions());
        source.Freeze();
        return source;
    }
}
