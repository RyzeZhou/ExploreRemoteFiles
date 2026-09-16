using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace RemoteFsClient.Services;

/// <summary>任务栏进度（ITaskbarList3）。操作队列窗口在任务栏上显示百分比，
/// 这样窗口被别的窗口盖住时用户仍能看到进度。</summary>
/// <remarks>
/// 红线：这只影响观感，任何失败（老系统、Shell 未就绪、COM 不可用）都必须被吞掉——
/// 绝不能因为画不出进度条而让一次真正的远程操作失败或中断。
/// 另：进度只会出现在**任务栏里有按钮**的窗口上，所以队列窗口必须 ShowInTaskbar=True。
/// </remarks>
internal static class TaskbarProgress
{
    public enum State
    {
        NoProgress = 0,
        Indeterminate = 1,
        Normal = 2,
        Error = 4,
        Paused = 8,
    }

    [ComImport, Guid("ea1afb91-9e28-4b86-90e9-9e9f8a5eefaf"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface ITaskbarList3
    {
        // ITaskbarList —— 顺序必须与 vtable 一致，不能只声明用到的那几个。
        void HrInit();
        void AddTab(IntPtr hwnd);
        void DeleteTab(IntPtr hwnd);
        void ActivateTab(IntPtr hwnd);
        void SetActiveAlt(IntPtr hwnd);
        // ITaskbarList2
        void MarkFullscreenWindow(IntPtr hwnd, [MarshalAs(UnmanagedType.Bool)] bool fullscreen);
        // ITaskbarList3
        void SetProgressValue(IntPtr hwnd, ulong completed, ulong total);
        void SetProgressState(IntPtr hwnd, State state);
    }

    [ComImport, Guid("56FDF344-FD6D-11d0-958A-006097C9A090")]
    private class TaskbarListInstance { }

    private static ITaskbarList3? _list;
    private static bool _failed;

    private static ITaskbarList3? Get()
    {
        if (_list is not null || _failed) return _list;
        try
        {
            var list = (ITaskbarList3)new TaskbarListInstance();
            list.HrInit();
            _list = list;
        }
        catch
        {
            _failed = true;   // 只试一次：失败就别每次都去戳 COM
        }
        return _list;
    }

    public static void Set(Window window, State state, ulong done = 0, ulong total = 0)
    {
        try
        {
            var hwnd = new WindowInteropHelper(window).Handle;
            if (hwnd == IntPtr.Zero) return;
            var list = Get();
            if (list is null) return;
            list.SetProgressState(hwnd, state);
            if (state == State.Normal && total > 0) list.SetProgressValue(hwnd, done, total);
        }
        catch
        {
            // 任务栏进度纯属锦上添花：失败不影响操作本身。
        }
    }

    public static void Clear(Window window) => Set(window, State.NoProgress);
}
