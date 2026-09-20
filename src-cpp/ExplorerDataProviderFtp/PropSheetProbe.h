/**************************************************************************
    PropSheetProbe.h - 属性页取证探针（2026-09-19，针对 Win11 实测症状）

    症状（**Win11 独有**，Win10 上正常）：
      「属性页在前台时，其他资源管理器窗口全被锁住，必须先关掉属性页才能操作。」

    机制上只可能是下面两种之一，探针的唯一目的就是把它们**分开**：

      1. 窗口被禁用（`EnableWindow(FALSE)`）—— 模态对话框的正常行为。
         问题不在"禁用"本身，而在**禁用的对象错了**：正常只该禁用当前浏览器
         窗口，若禁用到 explorer 的共享宿主/其他浏览器窗口，就是所有窗口一起灰。

      2. UI 线程被阻塞（hung）—— Win11 的资源管理器多标签**共用 UI 线程**，
         我们的回调里只要有一处同步等远程/等管道，该线程上**所有**窗口一起
         失去响应（Win11 的 DWM 还会把它们画成"变暗"）。

    Win10 不复现 ⇒ 差异要么在属性表宿主结构，要么在缓存冷热导致的阻塞路径。
    所以探针必须能回答"**是谁、在什么时候、以哪种方式**锁住的"：

      * `DumpChain()`：页面 → 根的完整窗口链（类名 / 线程 / enabled / owner /
        style），外加 GA_ROOT、GA_ROOTOWNER、前台窗口、活动窗口。
        Win11 的属性表宿主结构与 Win10 的差异一眼可见。

      * `Start()`：属性页存活期间起一个**采样线程**，每 300 ms：
          - 枚举本进程所有顶层窗口，比较 enabled 状态 → 谁被禁用了；
          - 对每个顶层窗口做 `SendMessageTimeout(WM_NULL)` → 谁 hung 了；
          - 只记录**状态变化**（不会刷屏），窗口消失也记一笔。

    采样线程是独立线程，即使 UI 线程卡死它照样跑 —— 这正是要点：
    卡死的线程没法给自己作证。

    开销：只在属性页存活期间运行，每轮枚举 + 一次 400ms 上限的探活；
    属性页释放即退出。想关掉它，设环境变量 ERF_PROBE_PROPSHEET=0。
**************************************************************************/
#pragma once

#include "ProbeLog.h"
// Module lifetime: this header starts its own worker thread (sampler), which
// must pin the DLL itself -- DllCanUnloadNow only counts COM objects.
void DllAddRef();
void DllRelease();
#include <strsafe.h>
#include <vector>
#include <string>
#include <wchar.h>

namespace prop_probe {

// ---------------------------------------------------------------------------
// 基础：把"一个窗口"打成一行
// ---------------------------------------------------------------------------
inline void LogWindow(PCWSTR tag, HWND h, PCWSTR note)
{
    WCHAR cls[96] = {};
    WCHAR title[160] = {};
    DWORD pid = 0, tid = 0;
    if (h)
    {
        GetClassNameW(h, cls, ARRAYSIZE(cls));
        GetWindowTextW(h, title, ARRAYSIZE(title));
        tid = GetWindowThreadProcessId(h, &pid);
    }
    ProbeLog(L"[PROBE] %s hwnd=%p cls='%s' pid=%lu tid=%lu enabled=%d visible=%d owner=%p parent=%p title='%s'%s%s",
             tag, (void *)h, cls, pid, tid,
             h ? IsWindowEnabled(h) : 0, h ? IsWindowVisible(h) : 0,
             h ? (void *)GetWindow(h, GW_OWNER) : NULL,
             h ? (void *)GetParent(h) : NULL,
             title,
             (note && note[0]) ? L" | " : L"", note ? note : L"");
}

// ---------------------------------------------------------------------------
// 窗口链 dump：属性页打开/关闭时各来一次
// ---------------------------------------------------------------------------
inline void DumpChain(HWND hDlg, PCWSTR tag)
{
    HWND fg = GetForegroundWindow();
    HWND act = GetActiveWindow();
    ProbeLog(L"[PROBE] ===== chain dump (%s) tid=%lu =====", tag ? tag : L"?", GetCurrentThreadId());
    LogWindow(L"foreground ", fg, L"");
    LogWindow(L"active     ", act, L"");

    HWND w = hDlg;
    for (int level = 0; w && level < 12; ++level)
    {
        WCHAR note[96] = {};
        StringCchPrintfW(note, ARRAYSIZE(note), L"level=%d", level);
        LogWindow(level == 0 ? L"page       " : L"ancestor   ", w, note);
        w = GetParent(w);
    }

    HWND root = hDlg ? GetAncestor(hDlg, GA_ROOT) : NULL;
    HWND rootOwner = hDlg ? GetAncestor(hDlg, GA_ROOTOWNER) : NULL;
    LogWindow(L"GA_ROOT    ", root, L"");
    LogWindow(L"GA_ROOTOWNER", rootOwner, L"");
    if (rootOwner)
        LogWindow(L"lastActivePopup", GetLastActivePopup(rootOwner), L"");
    ProbeLog(L"[PROBE] ===== chain dump end =====");
}

// ---------------------------------------------------------------------------
// 采样线程
// ---------------------------------------------------------------------------
struct WinRow
{
    HWND hwnd;
    BOOL enabled;
    BOOL hung;
    DWORD tid;
};

struct Sampler
{
    HANDLE thread;
    volatile LONG stop;
    HWND dlg;
    DWORD dlgTid;
    ULONGLONG started;
    BOOL active;
};

inline Sampler &State()
{
    static Sampler s = {};
    return s;
}

inline BOOL ProbeEnabled()
{
    static int cached = -1;
    if (cached < 0)
    {
        WCHAR v[8] = {};
        DWORD n = GetEnvironmentVariableW(L"ERF_PROBE_PROPSHEET", v, ARRAYSIZE(v));
        cached = (n > 0 && v[0] == L'0') ? 0 : 1;
    }
    return cached ? TRUE : FALSE;
}

inline BOOL CALLBACK CollectProc(HWND h, LPARAM lp)
{
    std::vector<WinRow> *out = reinterpret_cast<std::vector<WinRow> *>(lp);
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;      // 只看本进程（explorer）
    // 隐藏窗口（输入法 IME / MSCTFIME UI、提示气泡）常年 enabled=0，
    // 与"其他窗口变暗"无关，只会把真正有用的变化淹掉。
    if (!IsWindowVisible(h)) return TRUE;

    WinRow r = {};
    r.hwnd = h;
    r.enabled = IsWindowEnabled(h);
    r.tid = tid;
    DWORD_PTR res = 0;
    // 探活：SMTO_ABORTIFHUNG 让"已经卡死"的窗口立刻返回失败，不会把我们拖住。
    if (!SendMessageTimeoutW(h, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 400, &res))
        r.hung = (GetLastError() == ERROR_TIMEOUT) ? TRUE : FALSE;
    out->push_back(r);
    return TRUE;
}

inline const WinRow *FindRow(const std::vector<WinRow> &v, HWND h)
{
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].hwnd == h) return &v[i];
    return NULL;
}

inline DWORD WINAPI SamplerProc(LPVOID)
{
    Sampler &s = State();
    std::vector<WinRow> prev;
    ULONGLONG hungSince = 0;      // 持续卡死的起点，用来报"卡了多久"

    while (!InterlockedCompareExchange(&s.stop, 0, 0))
    {
        std::vector<WinRow> now;
        EnumWindows(CollectProc, (LPARAM)&now);

        // 1) 新出现 / enabled 或 hung 状态变化 / 消失
        for (size_t i = 0; i < now.size(); ++i)
        {
            const WinRow &r = now[i];
            const WinRow *p = FindRow(prev, r.hwnd);
            if (!p)
            {
                WCHAR note[128] = {};
                StringCchPrintfW(note, ARRAYSIZE(note), L"NEW (enabled=%d hung=%d)", (int)r.enabled, (int)r.hung);
                LogWindow(L"appear     ", r.hwnd, note);
                continue;
            }
            if (p->enabled != r.enabled)
            {
                WCHAR note[128] = {};
                StringCchPrintfW(note, ARRAYSIZE(note), L"ENABLED %d -> %d  <== 变暗/解锁就在这一刻",
                                 (int)p->enabled, (int)r.enabled);
                LogWindow(L"CHANGED    ", r.hwnd, note);
            }
            if (p->hung != r.hung)
            {
                WCHAR note[128] = {};
                StringCchPrintfW(note, ARRAYSIZE(note), L"HUNG %d -> %d (tid=%lu)",
                                 (int)p->hung, (int)r.hung, r.tid);
                LogWindow(L"CHANGED    ", r.hwnd, note);
                if (r.hung && !hungSince) hungSince = GetTickCount64();
                if (!r.hung) hungSince = 0;
            }
        }
        for (size_t i = 0; i < prev.size(); ++i)
            if (!FindRow(now, prev[i].hwnd))
            {
                WCHAR note[64] = {};
                StringCchPrintfW(note, ARRAYSIZE(note), L"GONE (enabled=%d hung=%d)", (int)prev[i].enabled, (int)prev[i].hung);
                LogWindow(L"gone       ", prev[i].hwnd, note);
            }

        // 2) 我们自己的属性对话框：它是不是还活着、还 enabled
        if (s.dlg && !IsWindow(s.dlg))
        {
            ProbeLog(L"[PROBE] our prop sheet dialog %p is GONE", (void *)s.dlg);
            s.dlg = NULL;
        }

        // 3) 卡死超过 2 秒就再报一次"还在卡"，免得只看到一行不知道持续多久
        if (hungSince && GetTickCount64() - hungSince > 2000)
        {
            for (size_t i = 0; i < now.size(); ++i)
                if (now[i].hung)
                    LogWindow(L"STILL HUNG ", now[i].hwnd,
                              (std::wstring(L"hung for ") + std::to_wstring(GetTickCount64() - hungSince) + L" ms").c_str());
            hungSince = GetTickCount64();   // 每 2 秒报一次
        }

        prev.swap(now);
        for (int i = 0; i < 30 && !InterlockedCompareExchange(&s.stop, 0, 0); ++i)
            Sleep(10);
    }
    ProbeLog(L"[PROBE] sampler thread exit");
    DllRelease();
    return 0;
}

// 属性页创建后调用（传页面的 hDlg）。重复调用会复用已有线程。
inline void Start(HWND hDlg)
{
    if (!ProbeEnabled()) return;
    Sampler &s = State();
    s.dlg = hDlg ? GetAncestor(hDlg, GA_ROOT) : NULL;
    s.dlgTid = s.dlg ? GetWindowThreadProcessId(s.dlg, NULL) : GetCurrentThreadId();
    if (s.thread)
    {
        ProbeLog(L"[PROBE] sampler already running, dlg=%p dlgTid=%lu", (void *)s.dlg, s.dlgTid);
        return;
    }
    InterlockedExchange(&s.stop, 0);
    s.started = GetTickCount64();
    DllAddRef();   // 2026-09-20: pin the module while the sampler runs
    s.thread = CreateThread(NULL, 0, SamplerProc, NULL, 0, NULL);
    if (s.thread)
    {
        ProbeLog(L"[PROBE] sampler started (dlg=%p dlgTid=%lu uiTid=%lu)",
                 (void *)s.dlg, s.dlgTid, GetCurrentThreadId());
    }
    else
    {
        DllRelease();
        ProbeLog(L"[PROBE] sampler FAILED to start, err=%lu", GetLastError());
    }
}

inline void Stop()
{
    Sampler &s = State();
    if (!s.thread) return;
    InterlockedExchange(&s.stop, 1);
    WaitForSingleObject(s.thread, 2000);
    CloseHandle(s.thread);
    s.thread = NULL;
    s.active = FALSE;
    ProbeLog(L"[PROBE] sampler stopped after %llu ms", GetTickCount64() - s.started);
}

// 属性页里某个**可能阻塞**的操作前后包一下：超过阈值就留证据。
// 用法：ProbeScope _s(L"PermPage WM_INITDIALOG");
struct ProbeScope
{
    PCWSTR tag;
    ULONGLONG t0;
    ULONGLONG warnMs;
    ProbeScope(PCWSTR t, ULONGLONG warn = 200) : tag(t), t0(GetTickCount64()), warnMs(warn) {}
    ~ProbeScope()
    {
        ULONGLONG dt = GetTickCount64() - t0;
        if (dt >= warnMs)
            ProbeLog(L"[PROBE] SLOW %s took %llu ms (tid=%lu)", tag, dt, GetCurrentThreadId());
    }
};

} // namespace prop_probe
