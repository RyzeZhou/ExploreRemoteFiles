using System.IO;
using System.Text;
using RemoteFsClient.Services;

namespace RemoteFsClient;

/// <summary>「操作队列」窗口的自检（`RemoteFsClient.exe --queue-selftest`）。
///
/// 为什么要有它：A2 的关键不是窗口长什么样，而是**多个条目并存时汇总算得对不对**——
/// 成功/失败/取消各几项、失败明细有没有带出来、进行中是否禁止关闭、
/// 任务栏进度该显示哪一种状态。这些要么是纯计数逻辑，要么只在"全部结束"那一刻
/// 才看得出来，靠肉眼看窗口既慢又容易漏。
///
/// 本自检**不显示窗口、不联网、不建连接**：只创建窗口对象，喂进几个条目的
/// 生命周期，然后把标题/汇总/条目状态打印出来并断言。窗口不 Show，
/// 所以任务栏进度拿不到 HWND（会被安全跳过）——那部分由真实操作覆盖。
/// </summary>
internal static class QueueSelfTest
{
    private static int _fail;
    private static int _pass;
    private static readonly List<string> Lines = new();

    /// <summary>证据文件：客户端是 WinExe，可能根本没有控制台句柄
    /// （`Console.OutputEncoding` 会抛"句柄无效"），所以结果一律落盘。</summary>
    internal static string LogPath => Path.Combine(Path.GetTempPath(), "erf-queue-selftest.log");

    private static void Say(string line)
    {
        Lines.Add(line);
        try { Console.WriteLine(line); } catch { /* 没有控制台就算了，文件里一定有 */ }
    }

    /// <summary>强制走一遍测量/排列 + 渲染优先级，让 DataTemplate 真正附加绑定。</summary>
    private static void Pump(System.Windows.Window window)
    {
        window.UpdateLayout();
        window.Dispatcher.Invoke(() => { }, System.Windows.Threading.DispatcherPriority.Render);
        window.UpdateLayout();
    }

    private static void Check(bool ok, string what)
    {
        if (ok) { _pass++; Say($"  PASS  {what}"); }
        else { _fail++; Say($"  FAIL  {what}"); }
    }

    public static int Run()
    {
        Say($"== 操作队列窗口自检（窗口显示在屏幕外、不联网） {DateTime.Now:yyyy-MM-dd HH:mm:ss}");
        try
        {
            Body();
        }
        catch (Exception ex)
        {
            _fail++;
            Say($"  FAIL  自检自身抛异常：{ex}");
        }
        Say($"\n{_pass} 通过, {_fail} 失败");
        try { File.WriteAllLines(LogPath, Lines, new UTF8Encoding(true)); } catch { }
        try { Console.WriteLine($"证据文件：{LogPath}"); } catch { }
        return _fail == 0 ? 0 : 1;
    }

    private static void Body()
    {
        var window = new RemoteOperationQueueWindow("WSL-SFTP");
        // **必须真的 Show 一次**：DataTemplate 只有在窗口显示、条目被实体化之后才会附加绑定。
        // 只创建窗口对象会漏掉"绑定到只读属性"这类只在附加时抛出的错误 ——
        // 实测正是它把常驻服务进程带崩的（进度条 Value 默认 TwoWay）。
        // 放到屏幕外、不进任务栏，避免打扰使用者。
        window.ShowInTaskbar = false;
        window.Left = -32000;
        window.Top = -32000;
        window.Show();

        Check(window.Title.StartsWith("操作队列：") || window.Title.StartsWith("Operation queue:"),
              $"标题带站点名：{window.Title}");

        // ── 两个条目并存：一个删除（有总量）、一个递归改权限（无总量）
        int cancelDelete = 0, cancelChmod = 0;
        var del = window.AddEntry("/home/zhou/AI_work/old", RemoteOperationQueueWindow.OperationKind.Delete,
                                  () => cancelDelete++);
        var chmod = window.AddEntry("/home/zhou/AI_work/data", RemoteOperationQueueWindow.OperationKind.Chmod,
                                    () => cancelChmod++);
        // 条目加完必须强制走一遍布局：ItemsControl 只有在生成条目容器时才附加
        // DataTemplate 里的绑定。少了这一步，XAML 绑定错误在自检里根本不会出现
        // （实测：TwoWay 绑定只读属性把常驻服务带崩过，而当时的自检却"全过"）。
        Pump(window);
        Check(window.Summary().Contains("2"), $"两个条目时汇总显示进行中数量：{window.Summary()}");
        var visible = window.VisibleEntries();
        Check(visible.Count == 2, $"条目列表里有 2 条（实际 {visible.Count}）");
        Check(visible[0].Contains("删除") && visible[1].Contains("改权限"),
              "两条分别标明「删除」与「递归改权限」");

        // ── 删除：先扫描（总量未知）后按总量回报
        del.Update(0, 0, "正在扫描 /home/zhou/AI_work/old");
        Check(del.IsIndeterminate, "总量未知时进度条是不确定态（扫描阶段）");
        del.Update(3, 10, "/home/zhou/AI_work/old/f3");
        Check(!del.IsIndeterminate && Math.Abs(del.Percent - 30) < 0.01, $"按总量换算百分比（{del.Percent:0.#}%）");
        del.Complete(true, false, null);
        Check(del.IsFinished && del.Succeeded && del.Percent == 100, "删除完成后条目为成功且 100%");

        // ── 改权限：无总量，最后部分失败
        chmod.Update(120, 0, "/home/zhou/AI_work/data/x");
        Check(chmod.IsIndeterminate, "无总量时保持不确定态（递归改权限就是这种）");
        chmod.Complete(false, false, "已修改 40 个目录 / 80 个文件，2 项失败：/a: Permission denied");
        Check(chmod.IsFinished && !chmod.Succeeded && !chmod.WasCancelled, "部分失败算「失败」而不是成功");
        string summary = window.Summary();
        Check(summary.Contains("1") && summary.Contains("失败"), $"汇总区分成功与失败：{summary}");
        Check(summary.Contains("Permission denied"), "汇总里带出失败明细（前 3 条）");
        Check(window.CanCloseNow(), "全部结束 → 允许关闭窗口");

        // ── 取消：单独一个条目，取消后应记为"已取消"而不是失败
        var cancelling = window.AddEntry("/home/zhou/AI_work/ui", RemoteOperationQueueWindow.OperationKind.Delete,
                                         () => { });
        Check(!window.CanCloseNow(), "有进行中的条目 → 禁止关闭窗口（避免误以为已停止）");
        cancelling.RequestCancel();
        cancelling.Complete(false, true, null);
        Check(cancelling.WasCancelled && !cancelling.Succeeded, "取消后记为「已取消」");
        Check(cancelDelete == 0 && cancelChmod == 0, "没有点过取消的条目不会被牵连取消");

        // ── 取消回调确实被调用
        int fired = 0;
        var toCancel = window.AddEntry("/home/zhou/AI_work/tmp", RemoteOperationQueueWindow.OperationKind.Delete,
                                       () => fired++);
        toCancel.RequestCancel();
        Check(fired == 1, "对条目点「取消」会调用它的取消回调，且只调一次");
        toCancel.RequestCancel();
        Check(fired == 1, "重复点取消不会重复触发（幂等）");
        toCancel.Complete(false, true, null);

        window.CloseForSelfTest();
    }
}
