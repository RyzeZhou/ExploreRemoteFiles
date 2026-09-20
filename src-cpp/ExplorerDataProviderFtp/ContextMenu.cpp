// Context menu for the FTP Microsoft-Core namespace.
// WinSCP-grade verbs: Open / Edit / Download / Copy-to-clipboard / path-name
// copies / server-side copy & move / rename / delete / custom commands / properties.
#include <windows.h>
#include <shlobj.h>
#include <shlobj_core.h>   // COPYENGINE_S_DONT_PROCESS_CHILDREN (sherrors.h)
#include <shlwapi.h>
#include <strsafe.h>
#include <string>
#include <time.h>
#include <shellapi.h>
#include <uxtheme.h>   // SetWindowTheme：属性页只读值框要关掉视觉样式
#include <commctrl.h>  // SetWindowSubclass/DefSubclassProc：接管 modeless 属性表的按钮
#include "FtpMeta.h"
#include "FtpSites.h"
#include "VscodeBridge.h"
#include "SizeFormat.h"
#include "Utils.h"
#include "resource.h"
#include "ProbeLog.h"
#include "PropSheetProbe.h"   // 属性页取证探针（Win11「锁住其他窗口」）
#include <new>

#define MENU_DELETE 0
#define MENU_PROPERTIES 1
#define MENU_COPY_NATIVE 2
#define MENU_COPY_FULL 3
#define MENU_OPEN 4
#define MENU_EDIT 5
#define MENU_DOWNLOAD 6
#define MENU_COPY_CLIP 7
#define MENU_COPY_NAME 8
#define MENU_RCOPY 9
#define MENU_RMOVE 10
#define MENU_RENAME 11
#define MENU_TERMINAL 12
#define MENU_TERM_WT 13
#define MENU_TERM_PWSH 14
#define MENU_TERM_VSCODE 15
#define MENU_CUSTOM_BASE 100

#define MYOBJID 0x1234
#define MAX_SEL 64
#define MAX_CUSTOM 16
#pragma pack(1)
typedef struct tagCompactItem {
    USHORT cb; WORD MyObjID; BYTE nLevel; BYTE nSize; BYTE nSides; BYTE cchName;
    BOOL fIsFolder; WCHAR szName[1];
} COMPACTITEM;
#pragma pack()

static void CopyName(const COMPACTITEM *item, PWSTR out, UINT cch)
{
    UINT i=0; while(i+1<cch && i<item->cchName && item->szName[i]) { out[i]=item->szName[i]; i++; } out[i]=0;
}
static BOOL IsOurs(PCUIDLIST_RELATIVE p)
{
    return p && p->mkid.cb >= FIELD_OFFSET(COMPACTITEM, szName)+sizeof(WCHAR) && ((const COMPACTITEM*)p)->MyObjID==MYOBJID;
}
static void PidlPath(PCIDLIST_ABSOLUTE abs, PWSTR out, UINT cch)
{
    // First IsOurs segment is the site name (level 1); skip it, join the rest.
    out[0]=0; PCUIDLIST_RELATIVE p=(PCUIDLIST_RELATIVE)abs; BOOL first=TRUE;
    while(p && p->mkid.cb) {
        if(IsOurs(p)) { WCHAR name[256]; CopyName((const COMPACTITEM*)p,name,ARRAYSIZE(name));
            if(name[0] && !first) { StringCchCat(out,cch,L"/"); StringCchCat(out,cch,name); }
            first=FALSE;
        }
        p=ILNext(p);
    }
    if(!out[0]) StringCchCopy(out,cch,L"/");
}
static BOOL PidlSite(PCIDLIST_ABSOLUTE abs, PWSTR out, UINT cch)
{
    out[0]=0; PCUIDLIST_RELATIVE p=(PCUIDLIST_RELATIVE)abs;
    while(p && p->mkid.cb) {
        if(IsOurs(p)) { CopyName((const COMPACTITEM*)p,out,cch); return out[0]!=0; }
        p=ILNext(p);
    }
    return FALSE;
}
static void ApplySiteStartPath(PCWSTR site, PWSTR path, UINT cch)
{
    if (!site || !site[0] || !path || !path[0]) return;
    const FTPSITE *s = FtpSiteFind(site);
    if (!s || !s->startPath[0] || StrCmp(s->startPath, L"/") == 0) return;

    if (StrCmp(path, L"/") == 0)
    {
        StringCchCopy(path, cch, s->startPath);
        return;
    }

    WCHAR relative[512] = {}, base[256] = {};
    StringCchCopy(relative, ARRAYSIZE(relative), path);
    StringCchCopy(base, ARRAYSIZE(base), s->startPath);
    while (lstrlen(base) > 1 && base[lstrlen(base) - 1] == L'/')
        base[lstrlen(base) - 1] = L'\0';
    StringCchPrintf(path, cch, L"%s%s", base, relative);
}

static void JoinPath(PCWSTR folder, PCWSTR name, PWSTR out, UINT cch)
{
    if(folder[0]==L'/' && !folder[1]) StringCchPrintf(out,cch,L"/%s",name);
    else StringCchPrintf(out,cch,L"%s/%s",folder,name);
}
static void CopyTextToClipboard(HWND hwnd, PCWSTR text)
{
    if (!text || !text[0]) return;
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    size_t cch = wcslen(text) + 1;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, cch * sizeof(WCHAR));
    if (hMem)
    {
        PWSTR dst = (PWSTR)GlobalLock(hMem);
        if (dst) { StringCchCopy(dst, cch, text); GlobalUnlock(hMem); SetClipboardData(CF_UNICODETEXT, hMem); }
        else GlobalFree(hMem);
    }
    CloseClipboard();
}
// timeoutMs：等待 CLI 结束的上限，超时即终止子进程。默认 30 秒适合单条目操作；
// 递归遍历（chmodr）这类长任务在**工作线程**里调用时可以放大（UI 线程绝不能用长超时）。
static int RunCli(PCWSTR site, PCWSTR verb, PCWSTR p1, PCWSTR p2, std::string *captured,
                  DWORD timeoutMs = 30000)
{
    WCHAR cmd[2400];
    PCWSTR cli = GetCliPath();
    if (!cli || !cli[0])
    {
        ProbeLog(L"[FTP] RunCli: CLI path is empty");
        return -1;
    }
    if (p2 && p2[0])
        StringCchPrintf(cmd,ARRAYSIZE(cmd),L"\"%s\" %s \"%s\" \"%s\" \"%s\"",GetCliPath(),verb,site,p1,p2);
    else
        StringCchPrintf(cmd,ARRAYSIZE(cmd),L"\"%s\" %s \"%s\" \"%s\"",GetCliPath(),verb,site,p1);
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE}; HANDLE rd=NULL,wr=NULL;
    if(captured && !CreatePipe(&rd,&wr,&sa,0)) return -1;
    if(captured) SetHandleInformation(rd,HANDLE_FLAG_INHERIT,0);
    STARTUPINFOW si={sizeof(si)}; if(captured){si.dwFlags=STARTF_USESTDHANDLES;si.hStdOutput=wr;si.hStdError=wr;si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);}
    PROCESS_INFORMATION pi={}; BOOL ok=CreateProcessW(NULL,cmd,NULL,NULL,captured?TRUE:FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi);
    if(captured) CloseHandle(wr);
    if(!ok){
        ProbeLog(L"[FTP] RunCli CreateProcess failed err=%lu cmd='%s'", GetLastError(), cmd);
        if(rd)CloseHandle(rd);
        return -1;
    }
    if(captured){char b[4096];DWORD n=0;while(ReadFile(rd,b,sizeof(b),&n,NULL)&&n)captured->append(b,n);CloseHandle(rd);}
    DWORD wait=WaitForSingleObject(pi.hProcess,timeoutMs);
    if(wait==WAIT_TIMEOUT){
        ProbeLog(L"[FTP] RunCli timed out cmd='%s'", cmd);
        TerminateProcess(pi.hProcess,1);
        WaitForSingleObject(pi.hProcess,2000);
    }
    DWORD code=1;GetExitCodeProcess(pi.hProcess,&code);
    ProbeLog(L"[FTP] RunCli site='%s' verb='%s' p1='%s' p2='%s' wait=%lu exit=%u",
             site,verb,p1,p2?p2:L"",wait,code);
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return code==0?0:-1;
}

// 每次用户级传输操作一个批次 id：传输队列据此分组，取消时也只取消这一批
//（不能用固定的 "-"，否则取消一个会连坐其它菜单传输）。
static void MakeTransferBatchId(PWSTR out, UINT cch)
{
    static volatile LONG s_seq = 0;
    StringCchPrintfW(out, cch, L"%u-%llu-%ld",
                     (unsigned)GetCurrentProcessId(), GetTickCount64(), InterlockedIncrement(&s_seq));
}

// 递归改权限的异步入口（实现在文件后面的终端辅助区）。属性页在 UI 线程上调用它，
// 真正的工作交给常驻服务 + 工作线程，所以这里必须先声明。
static void StartChmodRecursiveAsync(PCWSTR site, PCWSTR path, PCWSTR modeOctal);

// Single refresh pipeline for EVERY successful remote mutation — right-click
// commands, background menu, drop target (Ctrl+V / drag-drop) all funnel here.
// UI thread only queues work (never touches cache/network/pipe); the worker
// prefetches the fresh listing into the cache and only then notifies the view,
// so the re-enumeration can never land on an empty cache (WinSCP-style).
//
// site+folder are the FULL remote path of the VIEWED directory, passed in
// explicitly by the caller (it already has them) — NEVER re-derived from the
// PIDL here, because PIDL paths lack the site StartPath and a prefetch would
// then LIST a non-existent path (see 53e6639 / 9aa5535 regression chain).
static void AfterRemoteMutation(PCWSTR site, PCWSTR folder, PIDLIST_ABSOLUTE notifyPidl)
{
    if (!notifyPidl) { FtpCacheClear(); return; }
    ProbeLog(L"[MUT] AfterRemoteMutation queue site='%s' path='%s'", site ? site : L"", folder ? folder : L"/");
    FtpRefreshDirBackground(site, folder, notifyPidl);
}

// Fast optimistic path for high-frequency mutations (new folder / paste /
// delete / rename): the caller already patched the cache via FtpCachePatch*;
// notify the view immediately (it hits the patched entry — no network) and
// quietly prefetch the real listing in the background to correct metadata.
static void RefreshLocalFast(PCWSTR site, PCWSTR folder, PIDLIST_ABSOLUTE pidl)
{
    ProbeLog(L"[MUT] optimistic refresh site='%s' path='%s'", site ? site : L"", folder ? folder : L"/");
    FtpNotifyUpdateDir(pidl);
    FtpPrefetchQuiet(site, folder);
}

// ---------------------------------------------------------------------------
// 通用后台 CLI 写操作（2026-09-20）
//
// 背景：`RunCli` 是 CreateProcessW + WaitForSingleObject（默认 30s），而且每次都是
// 新进程 + 新连接。在 Explorer 的 UI 线程上直接调用，就是一次几百毫秒到几秒的假死
//（属性页「确定」、右键「新建文件夹 / 重命名 / 移动到 / 创建副本」全都如此）。
// 用户定调：**所有传输、权限修改、删除都必须在后台进行**。
//
// 范式与 StartDeleteRemote / StartChmodRecursiveAsync / DownloadJobProc 一致：
// UI 线程只投递参数（全部拷贝成自己的副本），工作线程执行 CLI，成功后走统一的
// AfterRemoteMutation 刷新管线，失败用 NULL owner 弹窗（绝不碰可能已销毁的 hwnd）。
// ---------------------------------------------------------------------------
enum BgAfter
{
    BG_NONE = 0,
    BG_REFRESH,        // AfterRemoteMutation(refreshSite, refreshFolder, notify)
    BG_PATCH_ADD,      // 新建目录：FtpCachePatchAdd + 立即刷新
    BG_PATCH_RENAME,   // 重命名：FtpCachePatchRename + 立即刷新
};

struct BgCliStep
{
    WCHAR site[64];
    WCHAR verb[16];
    WCHAR p1[700];
    WCHAR p2[700];
};

struct BgCliJob
{
    std::vector<BgCliStep> steps;
    BgAfter after = BG_REFRESH;
    WCHAR refreshSite[64] = {};
    WCHAR refreshFolder[600] = {};
    WCHAR nameA[256] = {};        // PATCH_ADD: 新名；PATCH_RENAME: 旧名
    WCHAR nameB[256] = {};        // PATCH_RENAME: 新名
    BOOL isFolder = FALSE;        // PATCH_ADD
    PIDLIST_ABSOLUTE notify = NULL;
    WCHAR failText[200] = {};     // 空 = 只记日志、不弹窗
    DWORD timeoutMs = 30000;
};

static void BgAddStep(BgCliJob &job, PCWSTR site, PCWSTR verb, PCWSTR p1, PCWSTR p2)
{
    BgCliStep s = {};
    StringCchCopy(s.site, ARRAYSIZE(s.site), site ? site : L"");
    StringCchCopy(s.verb, ARRAYSIZE(s.verb), verb ? verb : L"");
    StringCchCopy(s.p1, ARRAYSIZE(s.p1), p1 ? p1 : L"");
    StringCchCopy(s.p2, ARRAYSIZE(s.p2), p2 ? p2 : L"");
    job.steps.push_back(s);
}

static DWORD WINAPI BgCliJobProc(LPVOID p)
{
    BgCliJob *j = static_cast<BgCliJob *>(p);
    BOOL ok = TRUE;
    try
    {
        for (auto const &s : j->steps)
        {
            if (RunCli(s.site, s.verb, s.p1, (s.p2[0] ? s.p2 : NULL), NULL, j->timeoutMs) != 0)
            {
                // 多条目任务（移动到…）要像原来一样把每一项都试一遍，只是最后如实报失败。
                ProbeLog(L"[BGCLI] step failed site='%s' verb='%s' p1='%s'", s.site, s.verb, s.p1);
                ok = FALSE;
            }
        }
    }
    catch (...) { ok = FALSE; }

    if (ok)
    {
        switch (j->after)
        {
        case BG_PATCH_ADD:
            FtpCachePatchAdd(j->refreshSite, j->refreshFolder, j->nameA, j->isFolder, 0);
            RefreshLocalFast(j->refreshSite, j->refreshFolder, j->notify);
            break;
        case BG_PATCH_RENAME:
            FtpCachePatchRename(j->refreshSite, j->refreshFolder, j->nameA, j->nameB);
            RefreshLocalFast(j->refreshSite, j->refreshFolder, j->notify);
            break;
        case BG_REFRESH:
        default:
            ProbeLog(L"[BGCLI] job done site='%s' path='%s' steps=%u", j->refreshSite, j->refreshFolder, (UINT)j->steps.size());
            AfterRemoteMutation(j->refreshSite, j->refreshFolder, j->notify);
            break;
        }
    }
    else if (j->failText[0])
    {
        MessageBoxW(NULL, j->failText, ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK | MB_ICONERROR);
    }

    if (j->notify) ILFree(j->notify);
    delete j;
    DllRelease();
    return 0;
}

// 投递一个后台 CLI 任务。job.notify 的所有权转移给工作线程（投递失败时本函数负责释放）。
static void StartBgCliJob(BgCliJob job)
{
    if (job.steps.empty()) { if (job.notify) ILFree(job.notify); return; }
    BgCliJob *j = new (std::nothrow) BgCliJob(job);
    if (!j) { if (job.notify) ILFree(job.notify); return; }
    DllAddRef();   // 2026-09-20: 线程自己 pin 模块
    HANDLE h = CreateThread(NULL, 0, BgCliJobProc, j, 0, NULL);
    if (h) CloseHandle(h);
    else { DllRelease(); if (j->notify) ILFree(j->notify); delete j; }
}

// Defined immediately after the transfer-source bridge below.  Keep this
// declaration here because that bridge needs the parent remote directory.
static void PathParent(PCWSTR full, PWSTR out, UINT cch);

// Context handed to the background delete thread. All strings and the view
// PIDL must be copied (the IShellItem passed into RemoveItem is only valid for
// the duration of that call); the thread owns them and frees them when done.
struct DeleteRemoteCtx
{
    WCHAR site[64];
    WCHAR full[600];
    WCHAR parent[600];
    WCHAR name[MAX_PATH];
    BOOL isFolder;                 // recursive delete when the target is a dir
    PIDLIST_ABSOLUTE notifyPidl;   // cloned, freed by the thread
    UINT_PTR seq;
};

static DWORD WINAPI DeleteRemoteThreadProc(LPVOID p)
{
    DeleteRemoteCtx *c = static_cast<DeleteRemoteCtx *>(p);
    ProbeLog(L"[XFER] async delete begin site='%s' full='%s' folder=%d seq=%u",
             c->site, c->full, c->isFolder, (UINT)c->seq);

    // This waits for the real remote deletion to complete inside the resident
    // service (which owns the provider session and shows the self progress
    // window). It runs on a worker thread, never on Explorer's UI thread.
    std::string bridgeReply;
    if (FtpBridgeDelete(c->site, c->full, c->isFolder, bridgeReply))
    {
        // Deletion truly finished on the server. Only now update the view —
        // never remove an entry from the list before its remote delete has
        // actually completed (ERF_RESIDENT_SERVICE_ARCHITECTURE §一致性).
        FtpCachePatchRemove(c->site, c->parent, c->name);
        RefreshLocalFast(c->site, c->parent, c->notifyPidl);
        ProbeLog(L"[XFER] async delete done site='%s' full='%s' seq=%u", c->site, c->full, (UINT)c->seq);
    }
    else
    {
        // Delete failed (or was cancelled from the resident-side progress
        // window). Deliberately do NOT patch the cache / notify the view: the
        // item must stay visible so the user can retry, and the resident-side
        // window already surfaced the error/cancelled state.
        ProbeLog(L"[XFER] async delete failed site='%s' full='%s' reply='%hs' seq=%u",
                 c->site, c->full, bridgeReply.c_str(), (UINT)c->seq);
    }

    if (c->notifyPidl) ILFree(c->notifyPidl);
    delete c;
    DllRelease();
    return 0;
}

// Bridge for Explorer's native IFileOperation Delete path.  The command bar
// obtains ITransferSource from the containing folder and calls RemoveItem (or
// RecycleItem) with an absolute ERF IShellItem.
//
// The native delete confirmation has already been accepted before this method
// is called.  Do not show another dialog here.
//
// IMPORTANT (deferred-delete design): this method does NOT wait for the remote
// delete to finish.  Actual deletion, progress and cancellation are owned by
// the resident service (and its own foreground progress window).  We only
// decode the requested item, hand it to a background thread that issues the
// bridge DELETE, and return instantly so Explorer's native transfer queue
// completes immediately instead of pinning a task (which is what made both the
// native pause/cancel and subsequent queued jobs hang).  A successful
// directory delete returns COPYENGINE_S_DONT_PROCESS_CHILDREN so the shell
// does not also enumerate and re-delete each child that is already gone.
HRESULT DeleteRemoteShellItem(IShellItem *psiSource, PIDLIST_ABSOLUTE notifyPidl,
                              TRANSFER_SOURCE_FLAGS flags)
{
    if (!psiSource) return E_INVALIDARG;

    PIDLIST_ABSOLUTE source = NULL;
    HRESULT hr = SHGetIDListFromObject(psiSource, &source);
    if (FAILED(hr) || !source)
    {
        ProbeLog(L"[XFER] Delete could not get source PIDL hr=0x%08X", hr);
        return FAILED(hr) ? hr : E_FAIL;
    }

    WCHAR site[64] = {}, full[600] = {}, parent[600] = {}, name[MAX_PATH] = {};
    BOOL isFolder = FALSE;
    BOOL valid = PidlSite(source, site, ARRAYSIZE(site));
    PidlPath(source, full, ARRAYSIZE(full));
    ApplySiteStartPath(site, full, ARRAYSIZE(full));
    PCUIDLIST_RELATIVE last = ILFindLastID(source);
    if (valid && IsOurs(last))
    {
        CopyName((const COMPACTITEM *)last, name, ARRAYSIZE(name));
        isFolder = ((const COMPACTITEM *)last)->fIsFolder;
    }
    ILFree(source);

    if (!valid || !site[0] || !name[0])
    {
        ProbeLog(L"[XFER] Delete source is not an ERF child valid=%d site='%s' full='%s' name='%s'", valid, site, full, name);
        return E_INVALIDARG;
    }

    PathParent(full, parent, ARRAYSIZE(parent));

    DeleteRemoteCtx *c = new (std::nothrow) DeleteRemoteCtx{};
    if (!c) return E_OUTOFMEMORY;
    StringCchCopy(c->site, ARRAYSIZE(c->site), site);
    StringCchCopy(c->full, ARRAYSIZE(c->full), full);
    StringCchCopy(c->parent, ARRAYSIZE(c->parent), parent);
    StringCchCopy(c->name, ARRAYSIZE(c->name), name);
    c->isFolder = isFolder;
    c->notifyPidl = notifyPidl ? ILCloneFull(notifyPidl) : NULL;
    static volatile UINT_PTR s_seq = 0;
    c->seq = ++s_seq;

    ProbeLog(L"[XFER] Delete remote item queued (async) site='%s' full='%s' parent='%s' name='%s' folder=%d flags=0x%08X seq=%u",
             site, full, parent, name, isFolder, flags, (UINT)c->seq);

    DllAddRef();   // 2026-09-20: pin the module until the delete worker returns
    HANDLE h = CreateThread(NULL, 0, DeleteRemoteThreadProc, c, 0, NULL);
    if (!h)
    {
        DllRelease();
        if (c->notifyPidl) ILFree(c->notifyPidl);
        delete c;
        return E_OUTOFMEMORY;
    }
    CloseHandle(h);

    // Hand back success to the shell transfer engine immediately.  For a
    // directory the whole subtree is being deleted by the resident service
    // (recursive=1), so tell the shell not to also walk and delete its
    // children — that redundant walk is what produced the "file already
    // deleted" 550 errors and the pinned-queue task.
    return isFolder ? COPYENGINE_S_DONT_PROCESS_CHILDREN : S_OK;
}

// Run the same public IFileOperation path that Explorer uses for its command
// bar Delete button.  This gives the context-menu command the same shell-owned
// confirmation and progress UI, and it reaches DeleteRemoteShellItem above via
// the folder's ITransferSource implementation.  That makes ITransferSource
// the sole authority for actual ERF deletion.
static HRESULT DeleteSelectionWithNativeFileOperation(HWND hwnd, IDataObject *data)
{
    if (!data) return E_INVALIDARG;

    IShellItemArray *items = NULL;
    HRESULT hr = SHCreateShellItemArrayFromDataObject(data, IID_PPV_ARGS(&items));
    if (FAILED(hr))
    {
        ProbeLog(L"[XFER] native delete: SHCreateShellItemArrayFromDataObject failed hr=0x%08X", hr);
        return hr;
    }

    IFileOperation *op = NULL;
    hr = CoCreateInstance(CLSID_FileOperation, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&op));
    if (SUCCEEDED(hr))
    {
        if (hwnd) op->SetOwnerWindow(hwnd);
        hr = op->DeleteItems(items);
        if (SUCCEEDED(hr)) hr = op->PerformOperations();
        BOOL aborted = FALSE;
        // Explorer builds do not agree on the HRESULT used when the user
        // dismisses the native confirmation.  GetAnyOperationsAborted is the
        // authoritative cancellation signal and must be queried even when
        // PerformOperations has already returned a failure HRESULT.
        HRESULT hrAbort = op->GetAnyOperationsAborted(&aborted);
        if (SUCCEEDED(hrAbort) && aborted) hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        op->Release();
    }
    items->Release();
    ProbeLog(L"[XFER] native delete completed hr=0x%08X", hr);
    return hr;
}

// IFileOperation uses both HRESULT_FROM_WIN32(ERROR_CANCELLED) and E_ABORT
// for a user declining/cancelling its own confirmation UI, depending on the
// Explorer build and the exact point of cancellation.  They are normal user
// choices, not remote failures, and must behave exactly like the top button.
static BOOL IsNativeDeleteCancelled(HRESULT hr)
{
    return hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) ||
           hr == HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED) || hr == E_ABORT;
}

// View directory for a property-page mutation = parent of the mutated path.
static void PathParent(PCWSTR full, PWSTR out, UINT cch)
{
    StringCchCopy(out, cch, (full && full[0]) ? full : L"/");
    WCHAR *slash = wcsrchr(out, L'/');
    if (slash && slash != out) *slash = 0;
    else if (slash == out) out[1] = 0;
}

// WinSCP.com location for "script" custom commands: registry
// (HKCU\Software\ExplorerRemoteFs\WinScpPath, set by the GUI client) -> common
// install paths -> PATH. Supports portable/green WinSCP installs.
static const WCHAR *GetWinScpPath()
{
    static WCHAR s_path[MAX_PATH] = {};
    if (!s_path[0])
    {
        DWORD cb = sizeof(s_path);
        LONG r = RegGetValueW(HKEY_CURRENT_USER, L"Software\\ExplorerRemoteFs", L"WinScpPath",
                              RRF_RT_REG_SZ, NULL, s_path, &cb);
        if (r != ERROR_SUCCESS || !s_path[0])
        {
            s_path[0] = 0;
            static const WCHAR *probes[] = {
                L"C:\\Program Files (x86)\\WinSCP\\WinSCP.com",
                L"C:\\Program Files\\WinSCP\\WinSCP.com",
            };
            for (const WCHAR *p : probes)
            {
                if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES)
                { StringCchCopyW(s_path, ARRAYSIZE(s_path), p); break; }
            }
        }
    }
    return s_path;
}

#define BIT(v) ((v)?BST_CHECKED:BST_UNCHECKED)
typedef struct RemoteMeta {
    WCHAR name[MAX_PATH]; WCHAR type[24]; WCHAR mode[16]; WCHAR owner[40]; WCHAR group[40];
    WCHAR size[80]; WCHAR mtime[32]; DWORD bits; ULONGLONG dwSize; DWORD dwMtime;
    DWORD dwUid; DWORD dwGid;          // 0 = unknown
    BOOL fIsFolder; BOOL fIsSymlink;
} REMOTEMETA;
static DWORD ParseModeFromLine(const WCHAR *fields[])
{
    DWORD mode = 0;
    if (!fields || !fields[1]) return 0;
    const WCHAR *m = fields[1];
    for (int i = 0; i < 9; i++) { if (m[i+1] != L'-') mode |= (0400 >> i); }
    return mode;
}
static void FormatModeString(DWORD mode, BOOL folder, BOOL symlink, PWSTR out, UINT cch)
{
    WCHAR type = symlink ? L'l' : (folder ? L'd' : L'-');
    StringCchPrintf(out, cch, L"%c%c%c%c%c%c%c%c%c%c", type,
        (mode & 0400) ? L'r' : L'-', (mode & 0200) ? L'w' : L'-', (mode & 0100) ? L'x' : L'-',
        (mode & 0040) ? L'r' : L'-', (mode & 0020) ? L'w' : L'-', (mode & 0010) ? L'x' : L'-',
        (mode & 0004) ? L'r' : L'-', (mode & 0002) ? L'w' : L'-', (mode & 0001) ? L'x' : L'-');
}
// 属性页的大小：口径 + **精确字节数**两者都给（见 SizeFormat.h：
// "1.2 MB (1,234,567 B)"）—— 列里只给人读的形式，别让精确值无处可查。
static void FormatSizeString(ULONGLONG size, BOOL folder, PWSTR out, UINT cch)
{
    ErfFormatSizeWithExact(size, folder, out, cch);
}
static void FormatMtimeString(DWORD mtime, PWSTR out, UINT cch)
{
    __time64_t t = (__time64_t)mtime; struct tm tmLocal;
    if (_localtime64_s(&tmLocal,&t)==0) StringCchPrintf(out,cch,L"%04d-%02d-%02d %02d:%02d",
        tmLocal.tm_year+1900, tmLocal.tm_mon+1, tmLocal.tm_mday, tmLocal.tm_hour, tmLocal.tm_min);
    else StringCchCopy(out,cch,L"-");
}

// 从"已经拿到的列表"里填元数据 —— 走网络和走缓存两条路共用这一段。
static BOOL ReadRemoteMetaFromList(const std::vector<FTPENTRY> &entries, PCWSTR name, REMOTEMETA *meta)
{
    for (auto const &item : entries)
    {
        if (0 != StrCmp(item.szName, name)) continue;
        ZeroMemory(meta, sizeof(*meta));
        StringCchCopy(meta->name, ARRAYSIZE(meta->name), item.szName);
        StringCchCopy(meta->owner, ARRAYSIZE(meta->owner), item.szOwner);
        StringCchCopy(meta->group, ARRAYSIZE(meta->group), item.szGroup);
        meta->bits = item.dwMode;
        meta->fIsFolder = item.fIsFolder;
        meta->fIsSymlink = item.fIsSymlink;
        meta->dwUid = item.dwUid;
        meta->dwGid = item.dwGid;
        WCHAR mode[16];
        FormatModeString(item.dwMode, item.fIsFolder, item.fIsSymlink, mode, ARRAYSIZE(mode));
        StringCchCopy(meta->mode, ARRAYSIZE(meta->mode), mode);
        meta->dwSize = item.dwSize;
        meta->dwMtime = item.dwMtime;
        StringCchCopy(meta->type, ARRAYSIZE(meta->type),
            item.fIsSymlink ? ExplorerText(L"type.symbolic_link", L"符号链接", L"Symbolic Link") : (item.fIsFolder ? ExplorerText(L"type.folder", L"文件夹", L"Folder") : ExplorerText(L"type.file", L"文件", L"File")));
        FormatSizeString(item.dwSize, item.fIsFolder, meta->size, ARRAYSIZE(meta->size));
        FormatMtimeString(item.dwMtime, meta->mtime, ARRAYSIZE(meta->mtime));
        return TRUE;
    }
    return FALSE;
}

// **可能阻塞**：会同步拉远程列表（FtpListCachedAll 走命名管道，冷目录最坏几秒）。
// 只给"用户明确在等"的路径用（例如右键菜单里点了「属性」再弹模态框）。
static BOOL ReadRemoteMeta(PCWSTR site, PCWSTR folder, PCWSTR name, REMOTEMETA *meta)
{
    if (!name || !name[0]) return FALSE;
    std::vector<FTPENTRY> entries;
    if (!FtpListCachedAll(site, folder, entries)) return FALSE;
    return ReadRemoteMetaFromList(entries, name, meta);
}

// **绝不阻塞**：只读内存 + 磁盘快照（FtpCachePeekAllOrDisk 是纯本地读，带 8MB 上限，
// 不碰网络也不碰管道）。属性页必须用这个 ——
// AddPages 跑在 Explorer 的 UI 线程上，同步等远程会让整个 shell 无响应，
// 用户看到的就是"其他资源管理器窗口一起变暗、必须先关掉属性页"。
static BOOL ReadRemoteMetaCached(PCWSTR site, PCWSTR folder, PCWSTR name, REMOTEMETA *meta)
{
    if (!name || !name[0]) return FALSE;
    std::vector<FTPENTRY> entries;
    if (!FtpCachePeekAllOrDisk(site, folder, entries)) return FALSE;
    return ReadRemoteMetaFromList(entries, name, meta);
}

// ---- chmod write-back: properties dialog + prompt dialog -------------------

typedef struct {
    REMOTEMETA meta;
    WCHAR site[64];
    WCHAR path[600];
    PIDLIST_ABSOLUTE notify;
    BOOL canSetOwner;
    BOOL modeless;  // heap-owned only for background-menu windows
    // ---- 属性页异步元数据（2026-09-19）----
    // 冷缓存时 AddPages 只拿到空元数据（绝不能同步等远程），页面先用空值开出来，
    // 后台线程取回真实值后再 PostMessage 刷新。这样"打开属性页"永远是毫秒级，
    // 但用户最终看到的仍然是准确信息。
    WCHAR folder[600];
    WCHAR name[MAX_PATH];
    HWND  dlg;                 // 页面窗口，WM_INITDIALOG 时填
    volatile LONG metaReady;   // 后台线程已把元数据写回（1 = 可刷新）
    volatile LONG refs;        // 页面 + 后台线程共同持有，归零才 free
} PROPMETA;

// 页面与后台线程共享 PROPMETA 的所有权：谁最后放手谁释放。
// （后台取元数据的线程可能比属性页活得久 —— 用户点开就关，直接 CoTaskMemFree 会野指针。）
static void PropMetaAddRef(PROPMETA *pm)
{
    if (pm) InterlockedIncrement(&pm->refs);
}
static void PropMetaRelease(PROPMETA *pm)
{
    if (!pm) return;
    if (InterlockedDecrement(&pm->refs) != 0) return;
    if (pm->notify) CoTaskMemFree(pm->notify);
    CoTaskMemFree(pm);
}

// 属性页冷缓存 → 后台把真实元数据取回来（这一步允许阻塞：不在 UI 线程上）。
#define WM_APP_ERF_META_READY (WM_APP + 0x51)
struct MetaWarmCtx { PROPMETA *pm; };
static DWORD WINAPI MetaWarmThread(LPVOID p)
{
    MetaWarmCtx *c = (MetaWarmCtx *)p;
    PROPMETA *pm = c->pm;
    std::vector<FTPENTRY> entries;
    // 允许阻塞：这是后台线程。FtpListCachedAll 走管道 LIST（短请求），
    // 就算远程慢也只拖这个线程，不拖 Explorer 的 UI 线程。
    BOOL ok = FtpListCachedAll(pm->site, pm->folder, entries);
    ProbeLog(L"[PROBE] meta warm done site='%s' folder='%s' name='%s' ok=%d n=%u",
             pm->site, pm->folder, pm->name, (int)ok, (unsigned)entries.size());
    InterlockedExchange(&pm->metaReady, 1);
    if (pm->dlg && IsWindow(pm->dlg))
        PostMessageW(pm->dlg, WM_APP_ERF_META_READY, 0, 0);
    delete c;
    PropMetaRelease(pm);
    DllRelease();
    return 0;
}
static void MetaWarmStart(PROPMETA *pm)
{
    MetaWarmCtx *c = new (std::nothrow) MetaWarmCtx{ pm };
    if (!c) return;
    PropMetaAddRef(pm);
    DllAddRef();   // 2026-09-20: pin the module until the meta warm worker returns
    HANDLE h = CreateThread(NULL, 0, MetaWarmThread, c, 0, NULL);
    if (h) CloseHandle(h);
    else { DllRelease(); delete c; PropMetaRelease(pm); }
}

// FTP/FTPS have no standard owner/group mutation. SFTP can issue SETSTAT;
// whether the server ACL grants it is still confirmed when the user applies it.
static BOOL SiteCanSetOwner(PCWSTR site)
{
    const FTPSITE *s = FtpSiteFind(site);
    return s && 0 == StrCmpIW(s->type, L"sftp");
}

static void PermSetOwnerChangeVisible(HWND hDlg, BOOL visible)
{
    const int controls[] = { 3024, 3025, 3026, 3027 };
    for (int id : controls) ShowWindow(GetDlgItem(hDlg, id), visible ? SW_SHOW : SW_HIDE);
}

// 值框（3001~3007）去掉 WS_BORDER：只画底色还不够 —— 用户看到"输入框样式"
// 的根源是那个 1px 边框。去掉边框后它看起来就是普通文本，
// 但仍然是只读 EDIT，所以**可以拖动选中、Ctrl+C 复制**（这是保留 EDIT 的原因）。
static void PermMakeValueFieldsFlat(HWND hDlg)
{
    for (int id = 3001; id <= 3007; ++id)
    {
        HWND ctl = GetDlgItem(hDlg, id);
        if (!ctl) continue;
        LONG_PTR style = GetWindowLongPtrW(ctl, GWL_STYLE);
        if (style & WS_BORDER)
        {
            SetWindowLongPtrW(ctl, GWL_STYLE, style & ~WS_BORDER);
            SetWindowPos(ctl, NULL, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
        }
        // 关掉视觉样式：主题化的 EDIT 走 uxtheme 的 EP_EDITTEXT 绘制，那一版自带边框，
        // 光把样式位去掉在某些主题下仍会画出框（模板里也已声明 NOT WS_BORDER，两道保险）。
        SetWindowTheme(ctl, L"", L"");
    }
}

// 属性表的「确定/取消/应用」三个按钮是**属性表自己**建的，页面模板里没有它们，
// 也就没法用样式去掉。而我们的页面从不在中途提交（也从不调 PropSheet_Changed），
// 所以「应用」永远是灰的 —— 留一个按不动的按钮只会让人以为"哪里没生效"。
// 做法：从页面往上找属性表窗口，把 IDAPPLY(0x3021) 那个按钮藏掉。
// 只影响我们这一份属性表实例；提交一律走「确定」。
#define ERF_IDAPPLY 0x3021

// 权限页上那个提示气泡（见 PermAttachInputTooltip）。保存在这里是为了能在页面释放时
// DestroyWindow —— 以前创建完就再也不管，每打开一次属性页就漏一个顶层窗口。
static HWND g_permTip = NULL;

static void PermHideApplyButton(HWND hDlg)
{
    HWND w = hDlg;
    for (int depth = 0; depth < 4 && w; ++depth)
    {
        w = GetParent(w);
        if (!w) break;

        // ⚠ 只允许碰**真正的属性表对话框**（#32770 且属于本进程）。
        // 不校验的话，一旦宿主层级与预期不同（Win11 的 shell32 属性表宿主就是这样），
        // 循环会一路向上命中"属性表窗口的 owner"——那是资源管理器自己的浏览器窗口或
        // 共享 shell 窗口，对它 EnableWindow(FALSE)/ShowWindow(SW_HIDE) 的后果就是
        // "其他资源管理器窗口全部变暗，必须先关掉属性页才恢复"（2026-09-18 Win11 实测症状）。
        WCHAR cls[64] = {};
        GetClassNameW(w, cls, ARRAYSIZE(cls));
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (0 != _wcsicmp(cls, L"#32770") || pid != GetCurrentProcessId())
        {
            ProbeLog(L"[DIAG] PropSheet: skip non-dialog ancestor cls='%s' pid=%lu", cls, pid);
            continue;
        }

        HWND apply = GetDlgItem(w, ERF_IDAPPLY);
        if (apply)
        {
            // 只置灰（可恢复），**绝不 ShowWindow(SW_HIDE)**：隐藏不属于我们的窗口
            // 后果不可预期；而且"没有改动时「应用」本来就是灰的"正是 Windows 原生行为。
            EnableWindow(apply, FALSE);
            ProbeLog(L"[DIAG] PropSheet: apply button disabled hwnd=%p parent=%p", (void*)apply, (void*)w);
            return;
        }
    }
}

// 值框虽然只读，却是第一个可停靠控件：对话框一打开，焦点落在「名称」上，
// 而 EDIT 拿到焦点会**全选**文本 —— 用户看到的是一个蓝底高亮的名字，
// 像是"刚被选中准备改写"。这里把选择收起来（光标归 0），并把焦点交给对话框本身
// （不留高亮、也不让空格键误触某个勾选框），字段照样可点可复制。
static void PermClearValueSelection(HWND hDlg)
{
    for (int id = 3001; id <= 3007; ++id)
        SendDlgItemMessageW(hDlg, id, EM_SETSEL, 0, 0);
    SetFocus(hDlg);
}

// "所有者/组可填名称或数字 ID"这条提示从对话框正文里撤掉了（正文留白给信息行），
// 改成输入框上的 tooltip：鼠标停上去才出现，信息没丢、版面干净。
static void PermAttachInputTooltip(HWND hDlg)
{
    static std::wstring hint;      // tooltip 保存的是指针，必须由我们持有生命周期
    hint = ExplorerText(L"property.owner_input_hint",
                        L"可填名称或数字 ID（例：zhou 或 1000）；保留「名称 [ID]」原样即不修改。",
                        L"Accept a name or a numeric ID (e.g. zhou or 1000); keep \"name [ID]\" unchanged to skip.");

    // owner 必须是**顶层窗口**：传子窗口（hDlg）是非法 owner，Win11 的新属性表宿主
    // 对此更敏感 —— 提示气泡会被卷进激活链，表现为"其他资源管理器窗口像被模态挡住"。
    // 同时不再用 WS_EX_TOPMOST / HWND_TOPMOST：一个输入提示没有理由压在所有窗口之上。
    if (g_permTip) { DestroyWindow(g_permTip); g_permTip = NULL; }
    HWND owner = GetAncestor(hDlg, GA_ROOT);
    HWND tip = CreateWindowExW(0, TOOLTIPS_CLASSW, NULL,
                               WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                               owner ? owner : hDlg, NULL, g_hInst, NULL);
    if (!tip) return;
    g_permTip = tip;    const int ids[] = { 3024, 3025 };
    for (int id : ids)
    {
        HWND ctl = GetDlgItem(hDlg, id);
        if (!ctl) continue;
        TOOLINFOW ti = {};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = hDlg;
        ti.uId = (UINT_PTR)ctl;
        ti.lpszText = const_cast<PWSTR>(hint.c_str());
        SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}

// 只读值框的观感：值要**可选中复制**（所以不能改成静态文本），
// 但不该看着像能输入。只读的 EDIT 会把 WM_CTLCOLORSTATIC 发给父窗口
// （不是 WM_CTLCOLOREDIT），于是用**对话框自己的背景刷**把它的底画掉，
// 文字仍可拖动选择 / Ctrl+C 复制，视觉上等同静态文本。
// 只处理 3001~3007 这几个值框；真正的静态标签一律走默认绘制（保持原样，
// 免得主题化对话框里标签突然多出一个色块）。
// 返回值 0 表示"不是我负责的控件"，让对话框管理器按老路处理。
static INT_PTR PermColorReadOnlyValue(HWND hDlg, HWND ctl, WPARAM wp)
{
    int id = ctl ? GetDlgCtrlID(ctl) : 0;
    if (id < 3001 || id > 3007) return 0;
    SetBkMode((HDC)wp, TRANSPARENT);
    // 问对话框要它自己的背景刷（主题化时由 WM_CTLCOLORDLG 给出），
    // 这样值框的底色与周围**完全一致**，不会出现补丁感。
    LRESULT bg = SendMessageW(hDlg, WM_CTLCOLORDLG, wp, (LPARAM)hDlg);
    return bg ? bg : (INT_PTR)GetSysColorBrush(COLOR_BTNFACE);
}

static void PermSetChecks(HWND hDlg, DWORD mode)
{
    CheckDlgButton(hDlg,3011,BIT(mode&0400)); CheckDlgButton(hDlg,3012,BIT(mode&0200)); CheckDlgButton(hDlg,3013,BIT(mode&0100));
    CheckDlgButton(hDlg,3014,BIT(mode&0040)); CheckDlgButton(hDlg,3015,BIT(mode&0020)); CheckDlgButton(hDlg,3016,BIT(mode&0010));
    CheckDlgButton(hDlg,3017,BIT(mode&0004)); CheckDlgButton(hDlg,3018,BIT(mode&0002)); CheckDlgButton(hDlg,3019,BIT(mode&0001));
}
static DWORD PermCollectChecks(HWND hDlg)
{
    DWORD mode = 0;
    if(IsDlgButtonChecked(hDlg,3011))mode|=0400; if(IsDlgButtonChecked(hDlg,3012))mode|=0200; if(IsDlgButtonChecked(hDlg,3013))mode|=0100;
    if(IsDlgButtonChecked(hDlg,3014))mode|=0040; if(IsDlgButtonChecked(hDlg,3015))mode|=0020; if(IsDlgButtonChecked(hDlg,3016))mode|=0010;
    if(IsDlgButtonChecked(hDlg,3017))mode|=0004; if(IsDlgButtonChecked(hDlg,3018))mode|=0002; if(IsDlgButtonChecked(hDlg,3019))mode|=0001;
    return mode;
}
static void PermSyncChecksToOctal(HWND hDlg)
{
    WCHAR buf[8]; StringCchPrintf(buf,ARRAYSIZE(buf),L"%03o",PermCollectChecks(hDlg));
    SetDlgItemTextW(hDlg,3022,buf);
}
static void PermSyncOctalToChecks(HWND hDlg)
{
    WCHAR buf[8]; GetDlgItemTextW(hDlg,3022,buf,ARRAYSIZE(buf));
    if(!buf[0]) return;
    WCHAR *end=NULL; long v=wcstol(buf,&end,8);
    if(end==buf || *end) return;                 // not a valid octal number
    if((DWORD)v != PermCollectChecks(hDlg)) PermSetChecks(hDlg,(DWORD)v);
}

// Owner/Group 行显示 "名称 [ID]"；下面两个输入框回填**同一种写法**，并允许改写。
// 输入接受三种写法：名称、纯数字 ID、以及我们自己回填的 "名称 [ID]"。
// 归一化见 PermNormalizeOwnerInput；名字 → ID 的反查在服务程序侧
// （SftpFileSystem.SetOwner 用 /etc/passwd、/etc/group 反查）。
// 纯数字原样透传：root 可以指定一个还没有名字的 ID。
static void PermNormalizeOwnerInput(PCWSTR text, PWSTR out, UINT cch)
{
    out[0] = 0;
    if (!text) return;
    std::wstring s(text);
    size_t b = s.find_first_not_of(L" \t");
    if (b == std::wstring::npos) return;
    size_t e = s.find_last_not_of(L" \t");
    s = s.substr(b, e - b + 1);

    bool allDigits = true;
    for (WCHAR ch : s) if (ch < L'0' || ch > L'9') { allDigits = false; break; }
    if (allDigits) { StringCchCopyW(out, cch, s.c_str()); return; }

    size_t br = s.find(L'[');
    if (br == std::wstring::npos) { StringCchCopyW(out, cch, s.c_str()); return; }

    std::wstring head = s.substr(0, br);
    size_t hb = head.find_first_not_of(L" \t");
    if (hb != std::wstring::npos)
    {
        size_t he = head.find_last_not_of(L" \t");
        StringCchCopyW(out, cch, head.substr(hb, he - hb + 1).c_str());
        return;
    }
    // "[1000]" 这种没有名字的写法：取括号里的数字。
    size_t close = s.find(L']', br);
    if (close == std::wstring::npos) return;
    std::wstring inner = s.substr(br + 1, close - br - 1);
    size_t ib = inner.find_first_not_of(L" \t");
    if (ib == std::wstring::npos) return;
    size_t ie = inner.find_last_not_of(L" \t");
    StringCchCopyW(out, cch, inner.substr(ib, ie - ib + 1).c_str());
}

// 当前值在输入框里的规范写法：有名字用名字（服务端会反查成 ID），否则用数字。
// 两者都拿不到时留空（调用方不显示输入框）。
static void PermCurrentToken(const REMOTEMETA *m, BOOL user, PWSTR out, UINT cch)
{
    out[0] = 0;
    const WCHAR *name = user ? m->owner : m->group;
    DWORD id = user ? m->dwUid : m->dwGid;
    if (name && name[0]) { StringCchCopyW(out, cch, name); return; }
    if (id != 0xFFFFFFFF) StringCchPrintfW(out, cch, L"%u", id);
}

// 用户是否真的改了：把输入和**名字**与**数字 ID** 都比一遍。
// 少了这一步，"当前 zhou [1000]，用户把框里的 zhou 改成 1000"会被当成修改，
// 从而发出一次多余的 chown（服务器可能直接拒绝 NULL 变更）。
static BOOL PermTokenDiffers(PCWSTR token, const REMOTEMETA *m, BOOL user)
{
    if (!token[0]) return FALSE;
    const WCHAR *name = user ? m->owner : m->group;
    DWORD id = user ? m->dwUid : m->dwGid;
    if (name && name[0] && 0 == StrCmpW(token, name)) return FALSE;
    if (id != 0xFFFFFFFF)
    {
        WCHAR idText[16] = {};
        StringCchPrintfW(idText, ARRAYSIZE(idText), L"%u", id);
        if (0 == StrCmpW(token, idText)) return FALSE;
    }
    return TRUE;
}

static void PermInitOwnerGroup(HWND hDlg, const REMOTEMETA *m)
{
    WCHAR u[16] = {}, g[16] = {}, buf[160];
    if (m->dwUid != 0xFFFFFFFF) StringCchPrintf(u, ARRAYSIZE(u), L"%u", m->dwUid);
    if (m->dwGid != 0xFFFFFFFF) StringCchPrintf(g, ARRAYSIZE(g), L"%u", m->dwGid);
    StringCchPrintf(buf, ARRAYSIZE(buf), L"%s [%s]", m->owner[0] ? m->owner : L"-", u[0] ? u : L"-");
    SetDlgItemTextW(hDlg, 3004, buf);
    StringCchPrintf(buf, ARRAYSIZE(buf), L"%s [%s]", m->group[0] ? m->group : L"-", g[0] ? g : L"-");
    SetDlgItemTextW(hDlg, 3005, buf);

    WCHAR token[64] = {};
    PermCurrentToken(m, TRUE, token, ARRAYSIZE(token));
    if (token[0] && m->owner[0] && m->dwUid != 0xFFFFFFFF)
        StringCchPrintf(buf, ARRAYSIZE(buf), L"%s [%u]", m->owner, m->dwUid);   // 名称 [ID]
    else
        StringCchCopyW(buf, ARRAYSIZE(buf), token);
    SetDlgItemTextW(hDlg, 3024, buf);

    PermCurrentToken(m, FALSE, token, ARRAYSIZE(token));
    if (token[0] && m->group[0] && m->dwGid != 0xFFFFFFFF)
        StringCchPrintf(buf, ARRAYSIZE(buf), L"%s [%u]", m->group, m->dwGid);
    else
        StringCchCopyW(buf, ARRAYSIZE(buf), token);
    SetDlgItemTextW(hDlg, 3025, buf);
}

// Read the owner/group boxes and chown if either really changed.
// 名称由服务程序反查成 ID（纯数字直接透传）。
static void PermApplyChown(HWND hDlg, PROPMETA *pm)
{
    if (!pm->canSetOwner) return;
    WCHAR rawU[64] = {}, rawG[64] = {}, newU[64] = {}, newG[64] = {};
    GetDlgItemTextW(hDlg, 3024, rawU, ARRAYSIZE(rawU));
    GetDlgItemTextW(hDlg, 3025, rawG, ARRAYSIZE(rawG));
    PermNormalizeOwnerInput(rawU, newU, ARRAYSIZE(newU));
    PermNormalizeOwnerInput(rawG, newG, ARRAYSIZE(newG));

    BOOL changeU = PermTokenDiffers(newU, &pm->meta, TRUE);
    BOOL changeG = PermTokenDiffers(newG, &pm->meta, FALSE);
    if (!changeU && !changeG) return;

    // 协议是 "user:group"，名字里出现 ':' 会把字段劈开。
    if ((changeU && wcschr(newU, L':')) || (changeG && wcschr(newG, L':')))
    {
        MessageBoxW(hDlg, ExplorerText(L"error.owner_group_invalid", L"所有者/组不能包含冒号。", L"Owner/group cannot contain a colon."),
                    ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK | MB_ICONWARNING);
        return;
    }

    WCHAR spec[160];
    StringCchPrintf(spec, ARRAYSIZE(spec), L"%s:%s", changeU ? newU : L"-", changeG ? newG : L"-");
    // chown 走 CLI（新进程 + 新连接），绝不能在属性页的 UI 线程上等（2026-09-20）。
    (void)hDlg;   // 失败提示改由工作线程用 NULL owner 弹出
    BgCliJob job;
    BgAddStep(job, pm->site, L"chown", pm->path, spec);
    StringCchCopy(job.refreshSite, ARRAYSIZE(job.refreshSite), pm->site);
    PathParent(pm->path, job.refreshFolder, ARRAYSIZE(job.refreshFolder));
    job.notify = pm->notify ? ILCloneFull(pm->notify) : NULL;
    StringCchCopy(job.failText, ARRAYSIZE(job.failText),
        ExplorerText(L"error.owner_group_rejected", L"SFTP 服务器拒绝了所有者/组更新（名称必须能在远端解析，或直接填数字 ID）。", L"Owner/group update was rejected by the SFTP server (the name must resolve on the remote host, or use a numeric ID)."));
    StartBgCliJob(job);
}

static PCWSTR PropertyDialogTitle(const REMOTEMETA *meta)
{
    return meta && meta->fIsFolder
        ? ExplorerText(L"property.directory_properties", L"目录属性", L"Directory properties")
        : ExplorerText(L"property.file_properties", L"文件属性", L"File properties");
}
static void LocalizePermissionDialog(HWND hDlg)
{
    SetDlgItemTextW(hDlg, IDC_PROP_NAME, ExplorerText(L"property.name", L"名称：", L"Name:"));
    SetDlgItemTextW(hDlg, IDC_PROP_TYPE, ExplorerText(L"property.type", L"类型：", L"Type:"));
    SetDlgItemTextW(hDlg, IDC_PROP_PERMISSIONS, ExplorerText(L"property.permissions", L"权限：", L"Permissions:"));
    SetDlgItemTextW(hDlg, IDC_PROP_OWNER, ExplorerText(L"property.owner", L"所有者：", L"Owner:"));
    SetDlgItemTextW(hDlg, IDC_PROP_GROUP, ExplorerText(L"property.group", L"组：", L"Group:"));
    SetDlgItemTextW(hDlg, IDC_PROP_SIZE, ExplorerText(L"property.size", L"大小：", L"Size:"));
    SetDlgItemTextW(hDlg, IDC_PROP_MODIFIED, ExplorerText(L"property.modified", L"修改日期：", L"Modified:"));
    SetDlgItemTextW(hDlg, 3026, ExplorerText(L"property.new_owner", L"新所有者：", L"New owner:"));
    SetDlgItemTextW(hDlg, 3027, ExplorerText(L"property.new_group", L"新组：", L"New group:"));
    // 输入格式提示不写在正文里（用户要求去掉那一行），改挂 tooltip：见 PermAttachInputTooltip。
    SetDlgItemTextW(hDlg, IDC_PROP_PERMISSION_GROUP, ExplorerText(L"label.permissions", L"权限", L"Permissions"));
    SetDlgItemTextW(hDlg, IDC_PROP_OWNER_ROLE, ExplorerText(L"label.owner", L"所有者", L"Owner"));
    SetDlgItemTextW(hDlg, IDC_PROP_GROUP_ROLE, ExplorerText(L"label.group", L"组", L"Group"));
    SetDlgItemTextW(hDlg, IDC_PROP_OTHERS_ROLE, ExplorerText(L"label.others", L"其他", L"Others"));
    SetDlgItemTextW(hDlg, IDC_PROP_OCTAL_LABEL, ExplorerText(L"property.octal", L"八进制：", L"Octal:"));
    SetDlgItemTextW(hDlg, 3023, ExplorerText(L"property.recursive_subdirectories", L"递归应用到子目录", L"Recursive (subdirectories)"));
    const int reads[] = { 3011, 3014, 3017 }, writes[] = { 3012, 3015, 3018 }, execs[] = { 3013, 3016, 3019 };
    for (int id : reads) SetDlgItemTextW(hDlg, id, ExplorerText(L"property.read", L"读取", L"Read"));
    for (int id : writes) SetDlgItemTextW(hDlg, id, ExplorerText(L"property.write", L"写入", L"Write"));
    for (int id : execs) SetDlgItemTextW(hDlg, id, ExplorerText(L"property.execute", L"执行", L"Execute"));
    SetDlgItemTextW(hDlg, IDOK, ExplorerText(L"button.ok", L"确定", L"OK"));
    SetDlgItemTextW(hDlg, IDCANCEL, ExplorerText(L"button.cancel", L"取消", L"Cancel"));
}
static void LocalizeSiteDialog(HWND hDlg)
{
    SetDlgItemTextW(hDlg, IDC_SITE_NAME_LABEL, ExplorerText(L"property.name", L"名称：", L"Name:"));
    SetDlgItemTextW(hDlg, IDC_SITE_HOST_LABEL, ExplorerText(L"column.host", L"主机：", L"Host:"));
    SetDlgItemTextW(hDlg, IDC_SITE_PROTOCOL_LABEL, ExplorerText(L"property.protocol", L"协议：", L"Protocol:"));
    SetDlgItemTextW(hDlg, IDC_SITE_PORT_LABEL, ExplorerText(L"column.port", L"端口：", L"Port:"));
    SetDlgItemTextW(hDlg, IDC_SITE_USER_LABEL, ExplorerText(L"property.user", L"用户：", L"User:"));
    SetDlgItemTextW(hDlg, IDC_SITE_START_PATH_LABEL, ExplorerText(L"property.start_path", L"起始路径：", L"Start path:"));
    SetDlgItemTextW(hDlg, IDCANCEL, ExplorerText(L"button.close", L"关闭", L"Close"));
}
// 字段填充：WM_INITDIALOG 与"后台元数据回来了"两条路径共用同一段，
// 免得日子久了两边慢慢长歪。
static void PermFillMeta(HWND hDlg, const PROPMETA *pm)
{
    const REMOTEMETA *m = &pm->meta;
    SetWindowTextW(hDlg, PropertyDialogTitle(m));
    SetDlgItemTextW(hDlg,3001,m->name); SetDlgItemTextW(hDlg,3002,m->type);
    SetDlgItemTextW(hDlg,3003,m->mode); SetDlgItemTextW(hDlg,3006,m->size); SetDlgItemTextW(hDlg,3007,m->mtime);
    PermInitOwnerGroup(hDlg,m);
    PermSetOwnerChangeVisible(hDlg, pm->canSetOwner);
    PermSetChecks(hDlg,m->bits);
    PermSyncChecksToOctal(hDlg);
    EnableWindow(GetDlgItem(hDlg,3023), m->fIsFolder ? TRUE : FALSE);
}

// 后台元数据回来了（WM_APP_ERF_META_READY）：把真实值填进已经开着的对话框。
// 只在"当初确实是冷缓存"（meta.name 为空）时覆盖 —— 用户已经在改权限时
// 不要把界面推回去。
static void PermApplyAsyncMeta(HWND hDlg)
{
    PROPMETA *pm=(PROPMETA*)GetWindowLongPtrW(hDlg,DWLP_USER);
    if(!pm) return;
    if(!InterlockedCompareExchange(&pm->metaReady,0,0)) return;
    if(pm->meta.name[0]) return;                     // 当初就命中缓存，无需刷新
    REMOTEMETA fresh = {};
    if(!ReadRemoteMetaCached(pm->site, pm->folder, pm->name, &fresh)) return;
    pm->meta = fresh;
    PermFillMeta(hDlg, pm);
    ProbeLog(L"[PROBE] prop dialog refreshed from async meta name='%s' mode='%s'", fresh.name, fresh.mode);
}

static INT_PTR CALLBACK PermDlgProc(HWND hDlg,UINT msg,WPARAM wp,LPARAM lp)
{
    switch(msg){
    case WM_INITDIALOG:{
        prop_probe::ProbeScope _probe(L"PermDlg WM_INITDIALOG", 50);
        PROPMETA *pm=(PROPMETA*)lp; if(!pm)return TRUE;
        SetWindowLongPtrW(hDlg,DWLP_USER,(LONG_PTR)pm);
        pm->dlg = hDlg;                              // 后台元数据回来时 PostMessage 到这里
        prop_probe::DumpChain(hDlg, L"PermDlg init");
        prop_probe::Start(hDlg);
        LocalizePermissionDialog(hDlg);
        PermHideApplyButton(hDlg);
        PermMakeValueFieldsFlat(hDlg);
        if (pm->canSetOwner) PermAttachInputTooltip(hDlg);
        PermClearValueSelection(hDlg);
        PermFillMeta(hDlg, pm);
        PermApplyAsyncMeta(hDlg);                    // 冷缓存时后台已经先回来了
        return TRUE;}
    case WM_APP_ERF_META_READY:
        PermApplyAsyncMeta(hDlg);
        return TRUE;
    case WM_COMMAND:
        if(HIWORD(wp)==BN_CLICKED && LOWORD(wp)>=3011 && LOWORD(wp)<=3019){ PermSyncChecksToOctal(hDlg); return TRUE; }
        if(HIWORD(wp)==EN_CHANGE && LOWORD(wp)==3022){ PermSyncOctalToChecks(hDlg); return TRUE; }
        if(LOWORD(wp)==IDCANCEL){
            PROPMETA *pm=(PROPMETA*)GetWindowLongPtrW(hDlg,DWLP_USER);
            if(pm && pm->modeless) DestroyWindow(hDlg); else EndDialog(hDlg,IDCANCEL);
            return TRUE;}
        if(LOWORD(wp)==IDOK){
            PROPMETA *pm=(PROPMETA*)GetWindowLongPtrW(hDlg,DWLP_USER);
            if(pm){
                PermSyncOctalToChecks(hDlg);
                DWORD mode = PermCollectChecks(hDlg);
                BOOL recursive = IsDlgButtonChecked(hDlg,3023)!=0;
                if(mode != pm->meta.bits || recursive){
                    WCHAR modeStr[8]; StringCchPrintf(modeStr,ARRAYSIZE(modeStr),L"%03o",mode);
                    if(recursive){
                        // 递归改权限可能遍历上万个条目：**绝不能在 UI 线程上等**。
                        // 交给常驻服务（它带进度窗口和「取消」），本对话框立即关闭。
                        StartChmodRecursiveAsync(pm->site, pm->path, modeStr);
                    } else {
                        // 单个条目的 chmod 也不再同步：RunCli 会起新进程 + 新连接，
                        // 在属性页的 UI 线程上等就是一次假死（2026-09-20）。
                        BgCliJob job;
                        BgAddStep(job, pm->site, L"chmod", pm->path, modeStr);
                        StringCchCopy(job.refreshSite, ARRAYSIZE(job.refreshSite), pm->site);
                        PathParent(pm->path, job.refreshFolder, ARRAYSIZE(job.refreshFolder));
                        job.notify = pm->notify ? ILCloneFull(pm->notify) : NULL;
                        StringCchCopy(job.failText, ARRAYSIZE(job.failText),
                            ExplorerText(L"error.chmod_failed", L"权限修改失败。", L"Permission update failed."));
                        StartBgCliJob(job);
                    }
                }
                PermApplyChown(hDlg,pm);
                if(pm->modeless) DestroyWindow(hDlg); else EndDialog(hDlg,IDOK);
            }
            return TRUE;}
        break;
    case WM_CTLCOLORSTATIC:
    {
        // 只读值框（3001~3007）的观感：见 PermColorReadOnlyValue。
        INT_PTR br = PermColorReadOnlyValue(hDlg, (HWND)lp, wp);
        if (br) return br;
        break;
    }
    case WM_NCDESTROY:{
        prop_probe::Stop();
        PROPMETA *pm=(PROPMETA*)GetWindowLongPtrW(hDlg,DWLP_USER);
        if(pm && pm->modeless) PropMetaRelease(pm);
        SetWindowLongPtrW(hDlg,DWLP_USER,0);
        break;}
    }
    return FALSE;
}

// 所有自定义“属性”入口都使用下面的标准属性表页；不再创建 IDD_PERMBOX
// 那种无标签的自定义对话框。声明放在这里，让背景菜单助手可先调用它。
static void ShowRemotePropertiesSheetModeless(PCWSTR site, PCWSTR folder, PCWSTR name,
                                              PCWSTR fullPath, BOOL canSetOwner);
// Background-menu helpers. The folder PIDL represents the current directory,
// so its metadata is found by listing the parent directory and selecting its leaf.
static void ShowCurrentFolderProperties(HWND hwnd, PCWSTR site, PCWSTR folder)
{
    if (!site || !site[0] || !folder || !folder[0]) return;
    WCHAR full[600] = {}, parent[600] = {}, name[MAX_PATH] = {};
    StringCchCopy(full, ARRAYSIZE(full), folder);
    while (lstrlen(full) > 1 && full[lstrlen(full) - 1] == L'/') full[lstrlen(full) - 1] = L'\0';
    WCHAR *slash = wcsrchr(full, L'/');
    if (!slash || !slash[1]){
        MessageBoxW(hwnd, ExplorerText(L"info.root_no_parent", L"远程根目录没有可用于读取 Unix 元数据的父目录项。", L"The remote root has no parent entry from which to read Unix metadata."), ExplorerText(L"property.directory_properties", L"目录属性", L"Directory properties"), MB_OK | MB_ICONINFORMATION); return; }
    StringCchCopy(name, ARRAYSIZE(name), slash + 1);
    if (slash == full) StringCchCopy(parent, ARRAYSIZE(parent), L"/");
    else { *slash = L'\0'; StringCchCopy(parent, ARRAYSIZE(parent), full); }

    (void)hwnd;  // 不可作为 owner：Win11 会把 owned-popup 链关联到 Explorer。
    ShowRemotePropertiesSheetModeless(site, parent, name, folder, SiteCanSetOwner(site));
}

static void PopulateSiteInfo(HWND hDlg, PCWSTR site)
{
    const FTPSITE *s = FtpSiteFind(site);
    WCHAR buf[64] = {};
    SetDlgItemTextW(hDlg, 4001, (s && s->name[0]) ? s->name : site);
    SetDlgItemTextW(hDlg, 4002, s ? s->host : L"");
    SetDlgItemTextW(hDlg, 4003, s ? s->type : L"");
    if (s) StringCchPrintf(buf, ARRAYSIZE(buf), L"%d", s->port);
    SetDlgItemTextW(hDlg, 4004, buf);
    SetDlgItemTextW(hDlg, 4005, s ? s->user : L"");
    SetDlgItemTextW(hDlg, 4006, s ? s->startPath : L"");
}
typedef struct { WCHAR site[64]; } SITEINFOCTX;
static INT_PTR CALLBACK SiteInfoDlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    if(msg==WM_INITDIALOG){ SITEINFOCTX *ctx=(SITEINFOCTX*)lp; if(!ctx)return FALSE; SetWindowLongPtrW(hDlg,DWLP_USER,(LONG_PTR)ctx); SetWindowTextW(hDlg, ExplorerText(L"property.site_properties", L"站点属性", L"Site properties")); LocalizeSiteDialog(hDlg); PopulateSiteInfo(hDlg,ctx->site); return TRUE; }
    if(msg==WM_COMMAND && (LOWORD(wp)==IDCANCEL || LOWORD(wp)==IDOK)){ DestroyWindow(hDlg); return TRUE; }
    if(msg==WM_NCDESTROY){ SITEINFOCTX *ctx=(SITEINFOCTX*)GetWindowLongPtrW(hDlg,DWLP_USER); if(ctx)CoTaskMemFree(ctx); SetWindowLongPtrW(hDlg,DWLP_USER,0); }
    return FALSE;
}
static void ShowCurrentSiteInfo(HWND hwnd, PCWSTR site)
{
    if (!FtpSiteFind(site)){
        MessageBoxW(hwnd, ExplorerText(L"info.site_configuration_unavailable", L"当前站点配置已不可用。", L"The current site configuration is no longer available."), ExplorerText(L"property.site_properties", L"站点属性", L"Site properties"), MB_OK | MB_ICONINFORMATION); return; }
    SITEINFOCTX *ctx=(SITEINFOCTX*)CoTaskMemAlloc(sizeof(*ctx));
    if(!ctx)return;
    ZeroMemory(ctx,sizeof(*ctx)); StringCchCopy(ctx->site,ARRAYSIZE(ctx->site),site);
    HWND dlg=CreateDialogParamW(g_hInst,MAKEINTRESOURCEW(IDD_SITEBOX),hwnd,SiteInfoDlgProc,(LPARAM)ctx);
    if(dlg){ ShowWindow(dlg,SW_SHOWNORMAL); SetForegroundWindow(dlg); } else CoTaskMemFree(ctx);
}
typedef struct { WCHAR* buf; UINT cch; PCWSTR caption; PCWSTR initial; } PROMPTCTX;
static INT_PTR CALLBACK NameDlgProc(HWND h,UINT m,WPARAM w,LPARAM l)
{
    if(m==WM_INITDIALOG){PROMPTCTX*c=(PROMPTCTX*)l;SetWindowLongPtrW(h,DWLP_USER,(LONG_PTR)c);SetWindowTextW(h,c->caption);SetDlgItemTextW(h,3030,ExplorerText(L"property.name",L"名称：",L"Name:"));SetDlgItemTextW(h,IDOK,ExplorerText(L"button.ok",L"确定",L"OK"));SetDlgItemTextW(h,IDCANCEL,ExplorerText(L"button.cancel",L"取消",L"Cancel"));SetDlgItemTextW(h,3101,c->initial);return TRUE;}
    if(m==WM_COMMAND&&(LOWORD(w)==IDOK||LOWORD(w)==IDCANCEL)){
        if(LOWORD(w)==IDOK){PROMPTCTX*c=(PROMPTCTX*)GetWindowLongPtrW(h,DWLP_USER);GetDlgItemTextW(h,3101,c->buf,c->cch);}
        EndDialog(h,LOWORD(w));return TRUE;}
    return FALSE;
}
static BOOL PromptText(HWND parent, PCWSTR caption, PWSTR buf, UINT cch, PCWSTR initial)
{
    PROMPTCTX c={buf,cch,caption,initial?initial:L""};
    return DialogBoxParamW(g_hInst,MAKEINTRESOURCEW(IDD_NAMEBOX),parent,NameDlgProc,(LPARAM)&c)==IDOK && buf[0];
}

// ---- selection (multi-select aware) -----------------------------------------

typedef struct {
    WCHAR site[64];
    WCHAR folder[512];
    WCHAR names[MAX_SEL][256];
    int count;
    BOOL firstIsFolder;
    PIDLIST_ABSOLUTE notify;
} SELDATA;

static BOOL CollectSelection(IDataObject *data, SELDATA *out)
{
    out->count=0; out->site[0]=0; out->folder[0]=0; out->firstIsFolder=FALSE; out->notify=NULL;
    if(!data) return FALSE;
    FORMATETC f={(CLIPFORMAT)RegisterClipboardFormatW(CFSTR_SHELLIDLIST),NULL,DVASPECT_CONTENT,-1,TYMED_HGLOBAL};
    STGMEDIUM st={};
    if(FAILED(data->GetData(&f,&st))) return FALSE;
    CIDA *cida=(CIDA*)GlobalLock(st.hGlobal);
    BOOL ok=FALSE;
    if(cida && cida->cidl>0){
        PCIDLIST_ABSOLUTE parent=(PCIDLIST_ABSOLUTE)((BYTE*)cida+cida->aoffset[0]);
        PidlSite(parent,out->site,ARRAYSIZE(out->site));
        PidlPath(parent,out->folder,ARRAYSIZE(out->folder));
        ApplySiteStartPath(out->site, out->folder, ARRAYSIZE(out->folder));
        out->notify=ILCloneFull(parent);
        for(UINT i=1;i<=cida->cidl && out->count<MAX_SEL;i++){
            PCUIDLIST_RELATIVE child=(PCUIDLIST_RELATIVE)((BYTE*)cida+cida->aoffset[i]);
            if(IsOurs(child)){ const COMPACTITEM *item=(const COMPACTITEM*)child; if(out->count==0) out->firstIsFolder=item->fIsFolder; CopyName(item,out->names[out->count],ARRAYSIZE(out->names[0])); out->count++; }
        }
        // Site-picker items (level 0) have no site segment in the folder PIDL;
        // accept a non-empty selection regardless so their property sheet works.
        ok = out->count>0;
    }
    if(cida) GlobalUnlock(st.hGlobal);
    ReleaseStgMedium(&st);
    if(!ok && out->notify){ CoTaskMemFree(out->notify); out->notify=NULL; }
    return ok;
}

// ---- temp staging for open / edit / download / clipboard --------------------

static BOOL GetConfiguredDirectory(PCWSTR valueName, PCWSTR fallbackLeaf, PWSTR out, UINT cch)
{
    DWORD cb = cch * sizeof(WCHAR); out[0] = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\ExplorerRemoteFs", valueName, RRF_RT_REG_SZ, NULL, out, &cb) != ERROR_SUCCESS || !out[0]) {
        WCHAR local[MAX_PATH] = {}; if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local))) return FALSE;
        if (FAILED(StringCchPrintfW(out, cch, L"%s\\ExplorerRemoteFs\\%s", local, fallbackLeaf))) return FALSE;
    }
    if (!PathIsDirectoryW(out) && ERROR_SUCCESS != SHCreateDirectoryExW(NULL, out, NULL) && !PathIsDirectoryW(out)) return FALSE;
    return PathIsDirectoryW(out);
}
static BOOL TempDir(PCWSTR sub, PCWSTR site, PWSTR out, UINT cch)
{
    WCHAR root[MAX_PATH] = {}; if (!GetConfiguredDirectory(L"FileCachePath", L"FileCache", root, ARRAYSIZE(root))) return FALSE;
    if (FAILED(StringCchPrintfW(out, cch, L"%s\\%s\\%s\\", root, sub, site))) return FALSE;
    if(!PathIsDirectoryW(out)) SHCreateDirectoryExW(NULL,out,NULL);
    return PathIsDirectoryW(out);
}
static BOOL TempLocalPath(PCWSTR sub, PCWSTR site, PCWSTR name, PWSTR out, UINT cch)
{
    if(!TempDir(sub,site,out,cch)) return FALSE;
    StringCchCat(out,cch,name);
    return TRUE;
}
static BOOL GetConfiguredEditor(PWSTR out, UINT cch)
{
    if (!out || cch == 0) return FALSE;
    out[0] = 0;
    DWORD cb = cch * sizeof(WCHAR);
    // The control centre stores either an executable path (for example
    // Code.exe) or the built-in default.  Do not accept arguments here: the
    // staged file is always passed as exactly one ShellExecute parameter.
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\ExplorerRemoteFs", L"EditorPath",
                     RRF_RT_REG_SZ, NULL, out, &cb) != ERROR_SUCCESS || !out[0])
        StringCchCopyW(out, cch, L"notepad.exe");
    return TRUE;
}
static BOOL LaunchConfiguredEditor(HWND hwnd, PCWSTR local)
{
    WCHAR editor[MAX_PATH] = {};
    if (!GetConfiguredEditor(editor, ARRAYSIZE(editor))) return FALSE;
    HINSTANCE result = ShellExecuteW(hwnd, L"open", editor, local, NULL, SW_SHOWNORMAL);
    return (INT_PTR)result > 32;
}
static void CleanupDir(PCWSTR dir)
{
    // Best-effort delete of temp staging dir contents (files only, no recursion depth 1).
    WCHAR pat[MAX_PATH]; StringCchPrintf(pat,ARRAYSIZE(pat),L"%s*",dir);
    WIN32_FIND_DATAW fd={}; HANDLE h=FindFirstFileW(pat,&fd);
    if(h!=INVALID_HANDLE_VALUE){
        do { if(!(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)){ WCHAR p[MAX_PATH]; StringCchPrintf(p,ARRAYSIZE(p),L"%s%s",dir,fd.cFileName); DeleteFileW(p);} }
        while(FindNextFileW(h,&fd)); FindClose(h); }
}

// Edit watcher: poll local mtime; on a saved change, upload back to the
// server.  A deferred-create draft deliberately has no remote object until
// that first successful upload.
typedef struct {
    WCHAR site[64]; WCHAR remote[600]; WCHAR local[MAX_PATH];
    BOOL deferredCreate;
    PIDLIST_ABSOLUTE notify;
} EDITCTX;
static ULONGLONG LocalWriteStamp(PCWSTR local)
{
    HANDLE h = CreateFileW(local, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    FILETIME ft = {};
    if (h != INVALID_HANDLE_VALUE) { GetFileTime(h, NULL, NULL, &ft); CloseHandle(h); }
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}
static BOOL UploadEditedFile(EDITCTX *c, BOOL *remoteCreated)
{
    // The first saved revision claims the remote name without overwrite.  The
    // subsequent normal PUT can therefore only update the file we just
    // created, rather than silently replacing a document created by somebody
    // else while the local draft was open.
    if (c->deferredCreate && !*remoteCreated)
    {
        if (RunCli(c->site, L"touch", c->remote, NULL, NULL) != 0) return FALSE;
        *remoteCreated = TRUE;
    }
    // 回写走常驻服务传输队列（进度/取消与其它传输统一）。
    WCHAR batchId[64]; MakeTransferBatchId(batchId, ARRAYSIZE(batchId));
    std::string putReply;
    if (FtpBridgePut(c->site, c->remote, c->local, batchId, putReply) != FtpBridgeFetchState::Done) return FALSE;
    if (c->deferredCreate)
    {
        WCHAR parent[600] = {};
        PathParent(c->remote, parent, ARRAYSIZE(parent));
        PCWSTR name = PathFindFileNameW(c->remote);
        ULONGLONG size = 0;
        WIN32_FILE_ATTRIBUTE_DATA attributes = {};
        if (GetFileAttributesExW(c->local, GetFileExInfoStandard, &attributes))
            size = ((ULONGLONG)attributes.nFileSizeHigh << 32) | attributes.nFileSizeLow;
        FtpCachePatchAdd(c->site, parent, name, FALSE, size);
        RefreshLocalFast(c->site, parent, c->notify);
    }
    else FtpCacheClear();
    return TRUE;
}
static DWORD WINAPI EditWatch(LPVOID p)
{
    EDITCTX *c=(EDITCTX*)p;
    ULONGLONG lastT = LocalWriteStamp(c->local);
    BOOL remoteCreated = FALSE;
    ULONGLONG start=GetTickCount64();
    while(GetTickCount64()-start < 30*60*1000){
        Sleep(3000);
        if(!PathFileExistsW(c->local)) break;   // editor deleted temp file / closed
        ULONGLONG nowT = LocalWriteStamp(c->local);
        if(nowT!=lastT && nowT!=0){
            // small settle delay, then upload
            Sleep(1200);
            if (UploadEditedFile(c, &remoteCreated)) lastT = LocalWriteStamp(c->local);
        }
    }
    // Catch a save immediately before the editor/watch loop ended.  Unlike the
    // previous unconditional final PUT, an untouched new-file draft remains
    // entirely local and never creates an empty remote file.
    if (PathFileExistsW(c->local))
    {
        ULONGLONG finalT = LocalWriteStamp(c->local);
        if (finalT != 0 && finalT != lastT) UploadEditedFile(c, &remoteCreated);
    }
    DeleteFileW(c->local);
    if (c->notify) ILFree(c->notify);
    CoTaskMemFree(c);   // allocated via CoTaskMemAlloc
    DllRelease();
    return 0;
}
static BOOL StartEditWatch(PCWSTR site, PCWSTR remote, PCWSTR local,
                           BOOL deferredCreate, PIDLIST_ABSOLUTE notifyPidl)
{
    EDITCTX *c = (EDITCTX *)CoTaskMemAlloc(sizeof(EDITCTX));
    if (!c) return FALSE;
    StringCchCopy(c->site, ARRAYSIZE(c->site), site);
    StringCchCopy(c->remote, ARRAYSIZE(c->remote), remote);
    StringCchCopy(c->local, ARRAYSIZE(c->local), local);
    c->deferredCreate = deferredCreate;
    c->notify = notifyPidl ? ILCloneFull(notifyPidl) : NULL;
    DllAddRef();   // 2026-09-20: pin the module while the edit watcher runs (up to 30 min)
    HANDLE thread = CreateThread(NULL, 0, EditWatch, c, 0, NULL);
    if (thread) { CloseHandle(thread); return TRUE; }
    DllRelease();
    if (c->notify) ILFree(c->notify);
    CoTaskMemFree(c);
    return FALSE;
}
// ---- async transfer worker (2026-09-20: symptom "copy/download blocks Explorer") --
// Menu verbs (download / open / edit / copy-to-clipboard) used to run RunCli(get)
// synchronously on the Explorer UI thread, so any slow file froze the whole shell
// with no progress UI. Architecture says transfers stay off the shell thread, so
// the blocking GET phase now runs here on a worker thread. UI-thread-only steps
// (save dialog, temp-dir prep) stay inline in the callers; worker-thread message
// boxes use a NULL owner so a possibly-dead Explorer HWND is never touched.
struct DownloadJob
{
    std::wstring site;
    std::wstring remote;
    std::wstring local;
    int kind;
};
static DWORD WINAPI DownloadJobProc(LPVOID p)
{
    DownloadJob *j = static_cast<DownloadJob *>(p);
    // 走常驻服务传输队列（会话复用 + 进度 + 可取消），不再每次起一个 CLI 进程。
    WCHAR batchId[64]; MakeTransferBatchId(batchId, ARRAYSIZE(batchId));
    std::string bridgeReply;
    if (FtpBridgeFetch(j->site.c_str(), j->remote.c_str(), j->local.c_str(), batchId, bridgeReply) != FtpBridgeFetchState::Done)
    {
        MessageBoxW(NULL, ExplorerText(L"error.download_failed", L"下载失败。", L"Download failed."), ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK | MB_ICONERROR);
        delete j;
        DllRelease();
        return 1;
    }
    if (j->kind == 1)
    {
        if ((INT_PTR)ShellExecuteW(NULL, L"open", j->local.c_str(), NULL, NULL, SW_SHOWNORMAL) <= 32)
        {
            MessageBoxW(NULL, ExplorerText(L"error.open_failed", L"打开失败。", L"Open failed."), ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK | MB_ICONERROR);
            DeleteFileW(j->local.c_str());
        }
    }
    else if (j->kind == 2)
    {
        if (!LaunchConfiguredEditor(NULL, j->local.c_str()))
        {
            MessageBoxW(NULL, ExplorerText(L"error.open_failed", L"打开失败。", L"Open failed."), ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK | MB_ICONERROR);
            DeleteFileW(j->local.c_str());
        }
        else if (!StartEditWatch(j->site.c_str(), j->remote.c_str(), j->local.c_str(), FALSE, NULL))
            MessageBoxW(NULL, ExplorerText(L"error.edit_watch_failed", L"已打开文件，但无法启动自动上传监视。", L"The file was created and opened, but automatic upload monitoring could not start."), ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK | MB_ICONWARNING);
    }
    delete j;
    DllRelease();
    return 0;
};
static void StartDownloadJob(PCWSTR site, PCWSTR remote, PCWSTR local, int kind)
{
    DownloadJob *j = new (std::nothrow) DownloadJob();
    if (!j) return;
    j->site = site ? site : L""; j->remote = remote ? remote : L""; j->local = local ? local : L"";
    j->kind = kind;
    DllAddRef();   // 2026-09-20: pin the module until the download worker returns
    HANDLE h = CreateThread(NULL, 0, DownloadJobProc, j, 0, NULL);
    if (h) CloseHandle(h);
    else { DllRelease(); delete j; }
}
struct DownloadBatch
{
    std::wstring site;
    std::wstring dir;
    std::vector<std::pair<std::wstring, std::wstring>> files;
};
static DWORD WINAPI DownloadBatchProc(LPVOID p)
{
    DownloadBatch *b = static_cast<DownloadBatch *>(p);
    int ok = 0;
    WCHAR batchId[64]; MakeTransferBatchId(batchId, ARRAYSIZE(batchId));
    for (auto &f : b->files)
    {
        std::string bridgeReply;
        if (FtpBridgeFetch(b->site.c_str(), f.first.c_str(), f.second.c_str(), batchId, bridgeReply) == FtpBridgeFetchState::Done) ++ok;
    }
    WCHAR msg[512];
    StringCchPrintf(msg, ARRAYSIZE(msg), ExplorerText(L"info.downloaded_to", L"已下载 %d 个文件到：\n%s", L"Downloaded %d files to:\n%s"), ok, b->dir.c_str());
    MessageBoxW(NULL, msg, ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK | MB_ICONINFORMATION);
    delete b;
    DllRelease();
    return 0;
}
struct ClipJob
{
    std::wstring site;
    std::wstring dir;
    std::wstring folder;
    std::vector<std::wstring> names;
};
static DWORD WINAPI ClipJobProc(LPVOID p)
{
    ClipJob *j = static_cast<ClipJob *>(p);
    std::vector<std::wstring> paths;
    WCHAR local[MAX_PATH];
    WCHAR full[700];
    WCHAR batchId[64]; MakeTransferBatchId(batchId, ARRAYSIZE(batchId));
    for (auto &nm : j->names)
    {
        StringCchPrintf(local, ARRAYSIZE(local), L"%s%s", j->dir.c_str(), nm.c_str());
        JoinPath(j->folder.c_str(), nm.c_str(), full, ARRAYSIZE(full));
        std::string bridgeReply;
        if (FtpBridgeFetch(j->site.c_str(), full, local, batchId, bridgeReply) == FtpBridgeFetchState::Done) paths.push_back(local);
    }
    if (paths.empty())
    {
        MessageBoxW(NULL, ExplorerText(L"error.copy_to_clipboard_failed", L"无法下载选中的项目，未复制到剪贴板。", L"The selected item(s) could not be downloaded, so nothing was copied to the clipboard."), ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK | MB_ICONERROR);
    }
    else
    {
        SIZE_T sz = sizeof(DROPFILES) + 2;
        for (auto &t : paths) sz += (t.size() + 1) * sizeof(WCHAR);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sz);
        if (h)
        {
            DROPFILES *df = (DROPFILES*)GlobalLock(h);
            df->pFiles = sizeof(DROPFILES); df->fWide = TRUE; df->pt.x = 0; df->pt.y = 0;
            WCHAR *dst = (WCHAR*)((BYTE*)df + sizeof(DROPFILES));
            for (auto &t : paths) { StringCchCopy(dst, (sz - ((BYTE*)dst - (BYTE*)df)) / 2, t.c_str()); dst += t.size() + 1; }
            *dst = 0;
            GlobalUnlock(h);
            if (OpenClipboard(NULL)) { EmptyClipboard(); SetClipboardData(CF_HDROP, h); CloseClipboard(); }
            else GlobalFree(h);
        }
    }
    delete j;
    DllRelease();
    return 0;
}
static void OpenRemote(HWND hwnd, PCWSTR site, PCWSTR folder, PCWSTR name, BOOL edit)
{
    (void)hwnd;  // worker-thread messages use a NULL owner (see DownloadJobProc).
    WCHAR full[700]; JoinPath(folder,name,full,ARRAYSIZE(full));
    WCHAR local[MAX_PATH];
    if(!TempLocalPath(L"Open",site,name,local,ARRAYSIZE(local))) return;
    // The blocking GET runs on a worker thread (2026-09-20); Explorer returns at once.
    StartDownloadJob(site, full, local, edit ? 2 : 1);
}
// ── 默认下载目录（"即点即下"，不再弹保存对话框）──────────────────────────────
// 读 HKCU\Software\ExplorerRemoteFs\DownloadDir（REG_SZ）；缺省 = %USERPROFILE%\Downloads。
static BOOL DefaultDownloadDir(PWSTR out, UINT cch)
{
    out[0] = 0;
    HKEY k = NULL;
    if (ERROR_SUCCESS == RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\ExplorerRemoteFs", 0, KEY_READ, &k))
    {
        DWORD type = 0, cb = cch * sizeof(WCHAR);
        LONG r = RegQueryValueExW(k, L"DownloadDir", NULL, &type, (LPBYTE)out, &cb);
        RegCloseKey(k);
        if (r != ERROR_SUCCESS || type != REG_SZ) out[0] = 0;
    }
    if (!out[0])
    {
        WCHAR profile[MAX_PATH] = {};
        if (!SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, profile)) return FALSE;
        StringCchPrintfW(out, cch, L"%s\\Downloads", profile);
    }
    size_t n = wcslen(out);
    if (n && (out[n - 1] == L'\\' || out[n - 1] == L'/')) out[n - 1] = 0;
    CreateDirectoryW(out, NULL);
    return out[0] != 0;
}

// 目标目录里若已有同名文件，自动加 " (2)"、" (3)" … —— "即点即下"不能悄悄覆盖用户已有文件。
static void UniqueLocalPath(PCWSTR dir, PCWSTR name, PWSTR out, UINT cch)
{
    StringCchPrintfW(out, cch, L"%s\\%s", dir, name);
    if (!PathFileExistsW(out)) return;
    PCWSTR dot = PathFindExtensionW(name);
    std::wstring stem(name, (dot && *dot) ? (size_t)(dot - name) : wcslen(name));
    std::wstring ext = (dot && *dot) ? dot : L"";
    for (int i = 2; i < 1000; ++i)
    {
        StringCchPrintfW(out, cch, L"%s\\%s (%d)%s", dir, stem.c_str(), i, ext.c_str());
        if (!PathFileExistsW(out)) return;
    }
}

static void DownloadFiles(HWND hwnd, PCWSTR site, PCWSTR folder, PCWSTR *names, int count)
{
    (void)hwnd;   // 即点即下：不再弹保存对话框，直接下到默认下载目录
    WCHAR dir[MAX_PATH] = {};
    if (!DefaultDownloadDir(dir, ARRAYSIZE(dir)))
    {
        if (!TempDir(L"Download", site, dir, ARRAYSIZE(dir))) return;
    }
    if (count == 1)
    {
        WCHAR local[MAX_PATH] = {}; UniqueLocalPath(dir, names[0], local, ARRAYSIZE(local));
        WCHAR full[700]; JoinPath(folder, names[0], full, ARRAYSIZE(full));
        ProbeLog(L"[DL] menu download '%s' -> '%s'", full, local);
        StartDownloadJob(site, full, local, 0);
        return;
    }
    // Multiple: one worker thread downloads sequentially, then reports once.
    DownloadBatch *b = new (std::nothrow) DownloadBatch();
    if (!b) return;
    b->site = site ? site : L""; b->dir = dir;
    for(int i=0;i<count;i++){
        WCHAR full[700]; JoinPath(folder,names[i],full,ARRAYSIZE(full));
        WCHAR local[MAX_PATH] = {}; UniqueLocalPath(dir, names[i], local, ARRAYSIZE(local));
        b->files.emplace_back(full, local);
    }
    DllAddRef();   // 2026-09-20: pin the module until the batch download returns
    HANDLE h = CreateThread(NULL, 0, DownloadBatchProc, b, 0, NULL);
    if (h) CloseHandle(h);
    else { DllRelease(); delete b; }
}
static void CopyClipboard(HWND hwnd, PCWSTR site, PCWSTR folder, PCWSTR *names, int count)
{
    (void)hwnd;  // worker-thread messages use a NULL owner (see ClipJobProc).
    WCHAR dir[MAX_PATH];
    if(!TempDir(L"Clip",site,dir,ARRAYSIZE(dir))) return;
    CleanupDir(dir);
    // Downloads run on a worker thread (2026-09-20); the CF_HDROP offer is
    // built there after the files land, so Explorer never blocks on GET.
    ClipJob *j = new (std::nothrow) ClipJob();
    if (!j) return;
    j->site = site ? site : L""; j->dir = dir; j->folder = folder ? folder : L"";
    for (int k = 0; k < count; k++) j->names.push_back(names[k]);
    DllAddRef();   // 2026-09-20: pin the module until the clipboard worker returns
    HANDLE h = CreateThread(NULL, 0, ClipJobProc, j, 0, NULL);
    if (h) CloseHandle(h);
    else { DllRelease(); delete j; }
}enum COPYTARGET { COPY_ORIGINAL = 0, COPY_SAME_SITE = 1, COPY_OTHER_SITE = 2, COPY_LOCAL_FOLDER = 3 };
typedef struct {
    WCHAR sourceSite[64]; WCHAR sourceFolder[600]; PCWSTR *names; int count;
    COPYTARGET target; WCHAR targetSite[64]; WCHAR targetPath[700];
} COPYCTX;
static void CopyDialogSetTarget(HWND hDlg, COPYCTX *ctx, COPYTARGET target)
{
    CheckRadioButton(hDlg, IDC_COPY_ORIGINAL, IDC_COPY_LOCAL, IDC_COPY_ORIGINAL + (int)target);
    BOOL needsPath = target != COPY_ORIGINAL, needsSite = target == COPY_OTHER_SITE, isLocal = target == COPY_LOCAL_FOLDER;
    EnableWindow(GetDlgItem(hDlg, IDC_COPY_SITE_LABEL), needsSite);
    EnableWindow(GetDlgItem(hDlg, IDC_COPY_SITE), needsSite);
    EnableWindow(GetDlgItem(hDlg, IDC_COPY_PATH_LABEL), needsPath);
    EnableWindow(GetDlgItem(hDlg, IDC_COPY_PATH), needsPath);
    EnableWindow(GetDlgItem(hDlg, IDC_COPY_BROWSE), isLocal);
    if (target == COPY_SAME_SITE) SetDlgItemTextW(hDlg, IDC_COPY_PATH, ctx->sourceFolder);
    else if (target == COPY_OTHER_SITE) SetDlgItemTextW(hDlg, IDC_COPY_PATH, L"/");
    else if (target == COPY_LOCAL_FOLDER) {
        WCHAR profile[MAX_PATH] = {}; DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, ARRAYSIZE(profile));
        SetDlgItemTextW(hDlg, IDC_COPY_PATH, n && n < ARRAYSIZE(profile) ? profile : L"");
    }
}
static void CopyDialogBrowseLocal(HWND hDlg)
{
    BROWSEINFOW bi = {}; bi.hwndOwner = hDlg; bi.lpszTitle = ExplorerText(L"dialog.select_local_folder", L"选择本地目标文件夹", L"Select local destination folder");
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi);
    if (!id) return;
    WCHAR path[MAX_PATH] = {}; if (SHGetPathFromIDListW(id, path)) SetDlgItemTextW(hDlg, IDC_COPY_PATH, path);
    CoTaskMemFree(id);
}
static INT_PTR CALLBACK CopyDlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    COPYCTX *ctx = (COPYCTX *)GetWindowLongPtrW(hDlg, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG: {
        ctx = (COPYCTX *)lp; SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)ctx);
        SetWindowTextW(hDlg, ExplorerText(L"dialog.copy_to", L"复制到...", L"Copy to..."));
        SetDlgItemTextW(hDlg, IDC_COPY_SOURCE_LABEL, ExplorerText(L"copy.source_path", L"原文件路径：", L"Source remote path:"));
        SetDlgItemTextW(hDlg, IDC_COPY_DESTINATION, ExplorerText(L"copy.destination", L"目标位置", L"Destination"));
        SetDlgItemTextW(hDlg, IDC_COPY_ORIGINAL, ExplorerText(L"copy.original", L"原地生成副本", L"Create a copy in the original folder"));
        SetDlgItemTextW(hDlg, IDC_COPY_SAME_SITE, ExplorerText(L"copy.same_site", L"同站点路径", L"Same site"));
        SetDlgItemTextW(hDlg, IDC_COPY_OTHER_SITE, ExplorerText(L"copy.other_site", L"异站点路径", L"Other site"));
        SetDlgItemTextW(hDlg, IDC_COPY_LOCAL, ExplorerText(L"copy.local", L"本地路径", L"Local folder"));
        SetDlgItemTextW(hDlg, IDC_COPY_SITE_LABEL, ExplorerText(L"copy.target_site", L"目标站点：", L"Target site:"));
        SetDlgItemTextW(hDlg, IDC_COPY_PATH_LABEL, ExplorerText(L"copy.target_path", L"目标路径：", L"Target path:"));
        SetDlgItemTextW(hDlg, IDC_COPY_BROWSE, ExplorerText(L"button.browse", L"浏览...", L"Browse..."));
        SetDlgItemTextW(hDlg, IDOK, ExplorerText(L"button.ok", L"确定", L"OK")); SetDlgItemTextW(hDlg, IDCANCEL, ExplorerText(L"button.cancel", L"取消", L"Cancel"));
        WCHAR source[1200] = {}; if (ctx->count == 1) { WCHAR full[700] = {}; JoinPath(ctx->sourceFolder, ctx->names[0], full, ARRAYSIZE(full)); StringCchPrintf(source, ARRAYSIZE(source), L"%s:%s", ctx->sourceSite, full); }
        else StringCchPrintf(source, ARRAYSIZE(source), ExplorerText(L"copy.multiple_source", L"%s:%s（已选择 %d 项）", L"%s:%s (%d items selected)"), ctx->sourceSite, ctx->sourceFolder, ctx->count);
        SetDlgItemTextW(hDlg, IDC_COPY_SOURCE, source);
        FTPSITE sites[64] = {}; int n = FtpSitesGet(sites, ARRAYSIZE(sites));
        for (int i = 0; i < n; ++i) if (StrCmpI(sites[i].name, ctx->sourceSite) != 0) SendDlgItemMessageW(hDlg, IDC_COPY_SITE, CB_ADDSTRING, 0, (LPARAM)sites[i].name);
        if (SendDlgItemMessageW(hDlg, IDC_COPY_SITE, CB_GETCOUNT, 0, 0) > 0) SendDlgItemMessageW(hDlg, IDC_COPY_SITE, CB_SETCURSEL, 0, 0);
        CopyDialogSetTarget(hDlg, ctx, COPY_ORIGINAL); return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_COPY_ORIGINAL: case IDC_COPY_SAME_SITE: case IDC_COPY_OTHER_SITE: case IDC_COPY_LOCAL:
            if (HIWORD(wp) == BN_CLICKED) { CopyDialogSetTarget(hDlg, ctx, (COPYTARGET)(LOWORD(wp) - IDC_COPY_ORIGINAL)); return TRUE; } break;
        case IDC_COPY_BROWSE: if (HIWORD(wp) == BN_CLICKED) { CopyDialogBrowseLocal(hDlg); return TRUE; } break;
        case IDOK:
            if (IsDlgButtonChecked(hDlg, IDC_COPY_SAME_SITE)) ctx->target = COPY_SAME_SITE;
            else if (IsDlgButtonChecked(hDlg, IDC_COPY_OTHER_SITE)) ctx->target = COPY_OTHER_SITE;
            else if (IsDlgButtonChecked(hDlg, IDC_COPY_LOCAL)) ctx->target = COPY_LOCAL_FOLDER;
            else ctx->target = COPY_ORIGINAL;
            if (ctx->target == COPY_OTHER_SITE) {
                int sel = (int)SendDlgItemMessageW(hDlg, IDC_COPY_SITE, CB_GETCURSEL, 0, 0);
                if (sel == CB_ERR) { MessageBoxW(hDlg, ExplorerText(L"error.copy_other_site_required", L"请选择目标站点。", L"Select a target site."), ExplorerText(L"dialog.copy_to", L"复制到...", L"Copy to..."), MB_OK | MB_ICONWARNING); return TRUE; }
                SendDlgItemMessageW(hDlg, IDC_COPY_SITE, CB_GETLBTEXT, sel, (LPARAM)ctx->targetSite);
            } else StringCchCopy(ctx->targetSite, ARRAYSIZE(ctx->targetSite), ctx->sourceSite);
            if (ctx->target != COPY_ORIGINAL) GetDlgItemTextW(hDlg, IDC_COPY_PATH, ctx->targetPath, ARRAYSIZE(ctx->targetPath));
            if (ctx->target == COPY_SAME_SITE || ctx->target == COPY_OTHER_SITE) {
                if (ctx->targetPath[0] != L'/') { MessageBoxW(hDlg, ExplorerText(L"error.remote_path_required", L"远程目标路径必须以 / 开头。", L"The remote target path must start with /."), ExplorerText(L"dialog.copy_to", L"复制到...", L"Copy to..."), MB_OK | MB_ICONWARNING); return TRUE; }
            }
            if (ctx->target == COPY_LOCAL_FOLDER && !PathIsDirectoryW(ctx->targetPath)) { MessageBoxW(hDlg, ExplorerText(L"error.local_folder_required", L"请选择存在的本地文件夹。", L"Select an existing local folder."), ExplorerText(L"dialog.copy_to", L"复制到...", L"Copy to..."), MB_OK | MB_ICONWARNING); return TRUE; }
            EndDialog(hDlg, IDOK); return TRUE;
        case IDCANCEL: EndDialog(hDlg, IDCANCEL); return TRUE;
        } break;
    }
    return FALSE;
}
static BOOL PromptCopyTarget(HWND hwnd, COPYCTX *ctx)
{
    return DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_COPYBOX), hwnd, CopyDlgProc, (LPARAM)ctx) == IDOK;
}
static BOOL CopyToRemoteFolder(PCWSTR site, PCWSTR sourceFolder, PCWSTR name, PCWSTR targetFolder, BOOL duplicate)
{
    WCHAR src[700] = {}, dst[700] = {}, targetName[MAX_PATH] = {};
    JoinPath(sourceFolder, name, src, ARRAYSIZE(src));
    if (duplicate) StringCchPrintf(targetName, ARRAYSIZE(targetName), ExplorerText(L"name.copy_suffix", L"%s - 副本", L"%s - Copy"), name);
    else StringCchCopy(targetName, ARRAYSIZE(targetName), name);
    if (targetFolder[0] == L'/' && !targetFolder[1]) StringCchPrintf(dst, ARRAYSIZE(dst), L"/%s", targetName);
    else StringCchPrintf(dst, ARRAYSIZE(dst), L"%s/%s", targetFolder, targetName);
    return RunCli(site, L"dup", src, dst, NULL) == 0;
}
// 「复制到…」整体搬到工作线程：同站点是 dup、跨站点是 get+put、复制到本地是 get+CopyFile，
// 无论哪种都是**传输**，按用户定调绝不能占 Explorer 的 UI 线程（2026-09-20）。
struct BgCopyCtx
{
    WCHAR site[64];
    WCHAR folder[600];
    std::vector<std::wstring> names;
    COPYTARGET target;
    WCHAR targetSite[64];
    WCHAR targetPath[700];
    PIDLIST_ABSOLUTE notify;
};

static DWORD WINAPI BgCopyThreadProc(LPVOID p)
{
    BgCopyCtx *c = static_cast<BgCopyCtx *>(p);
    BOOL ok = TRUE;
    WCHAR batchId[64]; MakeTransferBatchId(batchId, ARRAYSIZE(batchId));
    try
    {
        for (auto const &nm : c->names)
        {
            WCHAR src[700] = {}; JoinPath(c->folder, nm.c_str(), src, ARRAYSIZE(src));
            if (c->target == COPY_ORIGINAL || c->target == COPY_SAME_SITE) {
                if (!CopyToRemoteFolder(c->site, c->folder, nm.c_str(), c->target == COPY_ORIGINAL ? c->folder : c->targetPath, c->target == COPY_ORIGINAL || StrCmpI(c->folder, c->targetPath) == 0)) ok = FALSE;
                continue;
            }
            REMOTEMETA meta = {}; if (!ReadRemoteMeta(c->site, c->folder, nm.c_str(), &meta) || meta.fIsFolder) { ok = FALSE; continue; }

            // 「复制到本地文件夹」：**直接下到目标目录**的临时名，再原子改名 —— 不再经
            // %LOCALAPPDATA%\...\FileCache 中转（那是双写：先下一份、再 CopyFile 一份）。
            // 这也让半成品落在目标位置（等价 WinSCP 的 .part 语义）。
            if (c->target == COPY_LOCAL_FOLDER)
            {
                WCHAR localTarget[MAX_PATH] = {};
                StringCchPrintf(localTarget, ARRAYSIZE(localTarget), L"%s\\%s", c->targetPath, nm.c_str());
                // 工作线程没有可用的 owner 窗口，覆盖确认用 NULL owner。
                if (PathFileExistsW(localTarget) && IDYES != MessageBoxW(NULL, ExplorerText(L"confirm.overwrite_local", L"目标位置已有同名文件，要覆盖吗？", L"A file with the same name already exists. Replace it?"), ExplorerText(L"dialog.copy_to", L"复制到...", L"Copy to..."), MB_YESNO | MB_ICONWARNING)) continue;
                WCHAR part[MAX_PATH + 16] = {};
                StringCchPrintf(part, ARRAYSIZE(part), L"%s.rfs-part", localTarget);
                DeleteFileW(part);
                std::string getReply;
                if (FtpBridgeFetch(c->site, src, part, batchId, getReply) != FtpBridgeFetchState::Done)
                {
                    ok = FALSE;
                    DeleteFileW(part);
                    continue;
                }
                if (!MoveFileExW(part, localTarget, MOVEFILE_REPLACE_EXISTING))
                {
                    ProbeLog(L"[COPY] rename .rfs-part failed err=%lu part='%s' target='%s'",
                             GetLastError(), part, localTarget);
                    DeleteFileW(part);
                    ok = FALSE;
                }
                continue;
            }

            // 跨站点复制：本机仍要中转（下载到缓存再上传），保持原样。
            WCHAR local[MAX_PATH] = {};
            if (!TempLocalPath(L"Copy", c->site, nm.c_str(), local, ARRAYSIZE(local))) { ok = FALSE; continue; }
            std::string getReply;
            if (FtpBridgeFetch(c->site, src, local, batchId, getReply) != FtpBridgeFetchState::Done) { ok = FALSE; continue; }
            {
                WCHAR remote[700] = {}; if (c->targetPath[0] == L'/' && !c->targetPath[1]) StringCchPrintf(remote, ARRAYSIZE(remote), L"/%s", nm.c_str()); else StringCchPrintf(remote, ARRAYSIZE(remote), L"%s/%s", c->targetPath, nm.c_str());
                std::string putReply;
                if (FtpBridgePut(c->targetSite, remote, local, batchId, putReply) != FtpBridgeFetchState::Done) ok = FALSE;
            }
            DeleteFileW(local);
        }
    }
    catch (...) { ok = FALSE; }

    if (!ok) MessageBoxW(NULL, ExplorerText(L"error.copy_failed", L"部分项目复制失败。跨站点和本地复制目前仅支持文件。", L"Some items could not be copied. Cross-site and local copies currently support files only."), ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK | MB_ICONERROR);
    else AfterRemoteMutation(c->site, c->folder, c->notify);

    if (c->notify) ILFree(c->notify);
    delete c;
    DllRelease();
    return 0;
}

static void ServerCopy(HWND hwnd, PCWSTR site, PCWSTR folder, PCWSTR *names, int count, PIDLIST_ABSOLUTE notifyPidl)
{
    COPYCTX ctx = {}; StringCchCopy(ctx.sourceSite, ARRAYSIZE(ctx.sourceSite), site); StringCchCopy(ctx.sourceFolder, ARRAYSIZE(ctx.sourceFolder), folder); ctx.names = names; ctx.count = count;
    if (!PromptCopyTarget(hwnd, &ctx)) return;
    BgCopyCtx *c = new (std::nothrow) BgCopyCtx();
    if (!c) return;
    StringCchCopy(c->site, ARRAYSIZE(c->site), site);
    StringCchCopy(c->folder, ARRAYSIZE(c->folder), folder);
    c->target = ctx.target;
    StringCchCopy(c->targetSite, ARRAYSIZE(c->targetSite), ctx.targetSite);
    StringCchCopy(c->targetPath, ARRAYSIZE(c->targetPath), ctx.targetPath);
    for (int i = 0; i < count; ++i) c->names.emplace_back(names[i]);
    c->notify = notifyPidl ? ILCloneFull(notifyPidl) : NULL;
    DllAddRef();   // 2026-09-20: 线程自己 pin 模块
    HANDLE h = CreateThread(NULL, 0, BgCopyThreadProc, c, 0, NULL);
    if (h) CloseHandle(h);
    else { DllRelease(); if (c->notify) ILFree(c->notify); delete c; }
}
static void ServerMove(HWND hwnd, PCWSTR site, PCWSTR folder, PCWSTR *names, int count, PIDLIST_ABSOLUTE notifyPidl)
{
    // Move all selected items to a target directory (WinSCP "Move to").
    WCHAR dst[700]; StringCchCopy(dst,ARRAYSIZE(dst),folder && folder[0] ? folder : L"/");
    if(!PromptText(hwnd,ExplorerText(L"dialog.move_to",L"移动到...（目标远程目录）",L"Move to... (target remote directory)"),dst,ARRAYSIZE(dst),dst)) return;
    // strip trailing slash for joining
    if(dst[0] && dst[wcslen(dst)-1]==L'/') dst[wcslen(dst)-1]=0;
    if(!dst[0]) StringCchCopy(dst,ARRAYSIZE(dst),L"/");
    // 逐项 rename 走后台：一次「移动到…」可能是几十个条目，UI 线程一秒都不能占（2026-09-20）。
    BgCliJob job;
    for (int i = 0; i < count; i++)
    {
        WCHAR src[700]; JoinPath(folder,names[i],src,ARRAYSIZE(src));
        WCHAR target[700];
        if(dst[0]==L'/' && !dst[1]) StringCchPrintf(target,ARRAYSIZE(target),L"/%s",names[i]);
        else StringCchPrintf(target,ARRAYSIZE(target),L"%s/%s",dst,names[i]);
        BgAddStep(job, site, L"rename", src, target);
    }
    StringCchCopy(job.refreshSite, ARRAYSIZE(job.refreshSite), site);
    StringCchCopy(job.refreshFolder, ARRAYSIZE(job.refreshFolder), folder);
    job.notify = notifyPidl ? ILCloneFull(notifyPidl) : NULL;
    StringCchCopy(job.failText, ARRAYSIZE(job.failText),
        ExplorerText(L"error.move_failed",L"移动失败。",L"Move failed."));
    StartBgCliJob(job);
}
static void DoRename(HWND hwnd, PCWSTR site, PCWSTR folder, PCWSTR name, PIDLIST_ABSOLUTE notifyPidl)
{
    WCHAR newName[256]; StringCchCopy(newName,ARRAYSIZE(newName),name);
    if(!PromptText(hwnd,ExplorerText(L"dialog.rename",L"重命名",L"Rename"),newName,ARRAYSIZE(newName),newName)) return;
    if(0==StrCmp(newName,name)) return;
    WCHAR src[700],dst[700]; JoinPath(folder,name,src,ARRAYSIZE(src)); JoinPath(folder,newName,dst,ARRAYSIZE(dst));
    // rename 走后台：RunCli 起新进程 + 新连接，UI 线程不能等（2026-09-20）。
    BgCliJob job;
    BgAddStep(job, site, L"rename", src, dst);
    StringCchCopy(job.refreshSite, ARRAYSIZE(job.refreshSite), site);
    StringCchCopy(job.refreshFolder, ARRAYSIZE(job.refreshFolder), folder);
    StringCchCopy(job.nameA, ARRAYSIZE(job.nameA), name);
    StringCchCopy(job.nameB, ARRAYSIZE(job.nameB), newName);
    job.after = BG_PATCH_RENAME;
    job.notify = notifyPidl ? ILCloneFull(notifyPidl) : NULL;
    StringCchCopy(job.failText, ARRAYSIZE(job.failText),
        ExplorerText(L"error.rename_failed",L"重命名失败。",L"Rename failed."));
    StartBgCliJob(job);
}

// ---- custom commands ---------------------------------------------------------
// %APPDATA%\ExplorerRemoteFs\custom-commands.json
// [ { "name": "...", "type": "local" | "script", "command": "..." } ]
// placeholders in command: {path} {name} {site} {full}
typedef struct { WCHAR name[128]; WCHAR type[16]; WCHAR command[1200]; } CUSTCMD;
static int LoadCustomCommands(CUSTCMD *out, int max)
{
    int n=0;
    WCHAR file[MAX_PATH];
    if(FAILED(SHGetFolderPathW(NULL,CSIDL_APPDATA,NULL,0,file))) return 0;
    StringCchCatW(file,MAX_PATH,L"\\ExplorerRemoteFs\\custom-commands.json");
    HANDLE h=CreateFileW(file,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
    if(h==INVALID_HANDLE_VALUE) return 0;
    std::string text; char buf[4096]; DWORD got=0;
    while(ReadFile(h,buf,sizeof(buf),&got,NULL)&&got) text.append(buf,got);
    CloseHandle(h);
    size_t pos=0;
    while(n<max && (pos=text.find('{',pos))!=std::string::npos){
        size_t end=text.find('}',pos); if(end==std::string::npos)break;
        std::string obj=text.substr(pos,end-pos); pos=end+1;
        auto getStr=[&](const char*key,WCHAR*out,UINT cch){
            std::string pat=std::string("\"")+key+"\"";
            size_t k=obj.find(pat); if(k==std::string::npos)return;
            k=obj.find(':',k+pat.size()); if(k==std::string::npos)return;
            k=obj.find('"',k); if(k==std::string::npos)return;
            size_t e=obj.find('"',k+1); if(e==std::string::npos)return;
            std::string val=obj.substr(k+1,e-k-1);
            int need=MultiByteToWideChar(CP_UTF8,0,val.c_str(),-1,NULL,0);
            if(need>0) MultiByteToWideChar(CP_UTF8,0,val.c_str(),-1,out,cch);
        };
        CUSTCMD c={};
        getStr("name",&c.name[0],ARRAYSIZE(c.name));
        if(!c.name[0]) continue;
        getStr("type",&c.type[0],ARRAYSIZE(c.type));
        if(!c.type[0]) StringCchCopyW(c.type,ARRAYSIZE(c.type),L"local");
        getStr("command",&c.command[0],ARRAYSIZE(c.command));
        out[n++]=c;
    }
    return n;
}
static void RunCustomCommand(HWND hwnd, PCWSTR site, PCWSTR folder, PCWSTR name, int idx)
{
    CUSTCMD cmds[MAX_CUSTOM]={};
    int n=LoadCustomCommands(cmds,MAX_CUSTOM);
    if(idx<0||idx>=n) return;
    CUSTCMD &c=cmds[idx];
    WCHAR full[700]; JoinPath(folder,name,full,ARRAYSIZE(full));
    // Substitute {path} {name} {site} {full}
    WCHAR cmd[2400]={};
    {
        std::wstring s=c.command;
        auto rep=[&](PCWSTR token,PCWSTR val){
            std::wstring t=token; size_t at=0;
            while((at=s.find(t,at))!=std::wstring::npos){ s.replace(at,t.size(),val); at+=wcslen(val); }
        };
        rep(L"{site}",site); rep(L"{name}",name); rep(L"{path}",folder); rep(L"{full}",full);
        StringCchCopyW(cmd,ARRAYSIZE(cmd),s.c_str());
    }
    if(0==StrCmpIW(c.type,L"script")){
        // Run through winscp.com: open site then run the command line(s)
        WCHAR ws[2600];
        StringCchPrintf(ws,ARRAYSIZE(ws),L"\"%s\" /command \"open \\\"%s\\\"\" \"%s\" \"close\" \"exit\"",GetWinScpPath(),site,cmd);
        STARTUPINFOW si={sizeof(si)}; PROCESS_INFORMATION pi={};
        if(CreateProcessW(NULL,ws,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi)){
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        }
    } else {
        // local command via ShellExecute
        HINSTANCE hr=ShellExecuteW(hwnd,NULL,cmd,NULL,NULL,SW_SHOWNORMAL);
        if((INT_PTR)hr<=32){ MessageBoxW(hwnd,ExplorerText(L"error.custom_command_failed",L"自定义命令执行失败。",L"Custom command failed."),ExplorerText(L"dialog.remote",L"远程操作",L"Remote"),MB_OK|MB_ICONERROR); }
    }
}

// ---- background (folder empty area) menu helpers ----------------------------

static void NewFolderRemote(HWND hwnd, PCWSTR site, PCWSTR folder, PIDLIST_ABSOLUTE notifyPidl)
{
    WCHAR name[256]; StringCchCopy(name, ARRAYSIZE(name), ExplorerText(L"dialog.new_folder_default", L"新建文件夹", L"New folder"));
    if (!PromptText(hwnd, ExplorerText(L"dialog.new_folder", L"新建文件夹", L"New folder"), name, ARRAYSIZE(name), name)) return;
    WCHAR full[700]; JoinPath(folder, name, full, ARRAYSIZE(full));
    // mkdir 走后台：RunCli 起新进程 + 新连接，右键菜单的 UI 线程不能等（2026-09-20）。
    BgCliJob job;
    BgAddStep(job, site, L"mkdir", full, NULL);
    StringCchCopy(job.refreshSite, ARRAYSIZE(job.refreshSite), site);
    StringCchCopy(job.refreshFolder, ARRAYSIZE(job.refreshFolder), folder);
    StringCchCopy(job.nameA, ARRAYSIZE(job.nameA), name);
    job.isFolder = TRUE;
    job.after = BG_PATCH_ADD;
    job.notify = notifyPidl ? ILCloneFull(notifyPidl) : NULL;
    StringCchCopy(job.failText, ARRAYSIZE(job.failText),
        ExplorerText(L"error.create_folder_failed", L"创建文件夹失败。", L"Failed to create folder."));
    StartBgCliJob(job);
}

// A new file is deliberately not implemented as a clipboard upload.  It
// begins as a private local draft only.  The first saved revision claims the
// remote name without overwrite, then the established edit watcher uploads it.
static void NewFileRemote(HWND hwnd, PCWSTR site, PCWSTR folder, PIDLIST_ABSOLUTE notifyPidl)
{
    WCHAR name[256] = {};
    StringCchCopy(name, ARRAYSIZE(name), ExplorerText(L"dialog.new_file_default", L"新建文本文档.txt", L"New Text Document.txt"));
    if (!PromptText(hwnd, ExplorerText(L"dialog.new_file", L"新建文件", L"New file"), name, ARRAYSIZE(name), name)) return;
    if (wcspbrk(name, L"\\/") || 0 == lstrcmpW(name, L".") || 0 == lstrcmpW(name, L".."))
    {
        MessageBoxW(hwnd, ExplorerText(L"error.invalid_file_name", L"文件名不能包含路径分隔符。", L"The file name cannot contain a path separator."),
                    ExplorerText(L"dialog.new_file", L"新建文件", L"New file"), MB_OK | MB_ICONWARNING);
        return;
    }
    WCHAR full[700] = {}, editDir[MAX_PATH] = {}, local[MAX_PATH] = {};
    JoinPath(folder, name, full, ARRAYSIZE(full));
    if (!TempDir(L"Edit", site, editDir, ARRAYSIZE(editDir)) ||
        FAILED(StringCchPrintfW(local, ARRAYSIZE(local), L"%s%08X_%s", editDir, GetTickCount(), name)))
    {
        MessageBoxW(hwnd, ExplorerText(L"error.create_file_failed", L"无法准备本地编辑缓存。", L"Unable to prepare the local editing cache."),
                    ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK | MB_ICONERROR);
        return;
    }
    HANDLE empty = CreateFileW(local, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (empty == INVALID_HANDLE_VALUE)
    {
        MessageBoxW(hwnd, ExplorerText(L"error.create_file_failed", L"无法创建本地编辑文件。", L"Unable to create the local editing file."),
                    ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(empty);
    if (!LaunchConfiguredEditor(hwnd, local))
    {
        MessageBoxW(hwnd, ExplorerText(L"error.open_failed", L"打开失败。", L"Open failed."), ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK | MB_ICONERROR);
        DeleteFileW(local);
        return;
    }
    if (!StartEditWatch(site, full, local, TRUE, notifyPidl))
        MessageBoxW(hwnd, ExplorerText(L"error.edit_watch_failed", L"已创建并打开文件，但无法启动自动上传监视。", L"The file was created and opened, but automatic upload monitoring could not start."),
                    ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK | MB_ICONWARNING);
}

// Core upload path — the ONE implementation behind the background-menu paste,
// the folder IDropTarget (Ctrl+V / drag-drop) and any future paste entry.
// The caller owns hdrop's lifetime (clipboard data must not be freed here).
// Upload worker: runs OFF the UI thread. Large transfers used to block the
// Explorer window for the whole CLI put (no progress UI), making the window
// appear frozen. The HDROP is consumed on the UI thread (valid only while the
// data object/clipboard is open); the worker owns plain path strings.
struct PasteJob
{
    WCHAR site[64];
    WCHAR folder[512];
    std::vector<std::wstring> files;
    PIDLIST_ABSOLUTE pidl;
};
static DWORD WINAPI PasteJobProc(LPVOID p)
{
    PasteJob *j = static_cast<PasteJob *>(p);
    BOOL ok = TRUE;
    WCHAR batchId[64]; MakeTransferBatchId(batchId, ARRAYSIZE(batchId));
    for (auto const &local : j->files)
    {
        WCHAR name[MAX_PATH];
        StringCchCopy(name, ARRAYSIZE(name), PathFindFileNameW(local.c_str()));
        WCHAR full[700]; JoinPath(j->folder, name, full, ARRAYSIZE(full));
        ProbeLog(L"[UPLOAD] put '%s' -> '%s'", local.c_str(), full);
        // 上传走常驻服务传输队列（会话复用 + 进度 + 可取消），不再起 CLI 进程。
        std::string putReply;
        if (FtpBridgePut(j->site, full, local.c_str(), batchId, putReply) != FtpBridgeFetchState::Done) ok = FALSE;
        else
        {
            ULONGLONG sz = 0;
            WIN32_FILE_ATTRIBUTE_DATA fa = {};
            if (GetFileAttributesExW(local.c_str(), GetFileExInfoStandard, &fa))
                sz = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
            FtpCachePatchAdd(j->site, j->folder, name, FALSE, sz);   // optimistic
        }
    }
    RefreshLocalFast(j->site, j->folder, j->pidl);
    if (!ok) MessageBoxW(NULL, ExplorerText(L"error.some_uploads_failed", L"部分文件上传失败。", L"Some files could not be uploaded."), ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK|MB_ICONERROR);
    if (j->pidl) ILFree(j->pidl);
    delete j;
    DllRelease();
    return 0;
}

static void PasteHdropToFolder(HWND hwnd, PCWSTR site, PCWSTR folder, HDROP hdrop, PIDLIST_ABSOLUTE notifyPidl)
{
    (void)hwnd;
    UINT n = DragQueryFileW(hdrop, 0xFFFFFFFF, NULL, 0);
    if (!n) return;
    PasteJob *j = new (std::nothrow) PasteJob();
    if (!j) return;
    StringCchCopy(j->site, ARRAYSIZE(j->site), site ? site : L"");
    StringCchCopy(j->folder, ARRAYSIZE(j->folder), folder ? folder : L"/");
    j->pidl = notifyPidl ? ILCloneFull(notifyPidl) : NULL;
    for (UINT k = 0; k < n; k++)
    {
        WCHAR local[MAX_PATH];
        if (DragQueryFileW(hdrop, k, local, ARRAYSIZE(local)))
            j->files.emplace_back(local);
    }
    if (j->files.empty()) { if (j->pidl) ILFree(j->pidl); delete j; return; }
    ProbeLog(L"[UPLOAD] queued %u file(s) -> '%s' (background)", (UINT)j->files.size(), j->folder);
    DllAddRef();   // keep the DLL loaded while the worker runs
    HANDLE h = CreateThread(NULL, 0, PasteJobProc, j, 0, NULL);
    if (h) CloseHandle(h);
    else
    {
        if (j->pidl) ILFree(j->pidl);
        delete j;
        DllRelease();
    }
}

// IDataObject entry used by the folder IDropTarget (Ctrl+V / drag-drop).
// Non-static: called from ExplorerDataProvider.cpp's CFolderDropTarget.
void PasteDataObjectToFolder(HWND hwnd, PCWSTR site, PCWSTR folder, IDataObject *pdo, PIDLIST_ABSOLUTE notifyPidl)
{
    if (!pdo) return;
    FORMATETC fmt = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM st = {};
    if (FAILED(pdo->GetData(&fmt, &st))) return;
    if (st.hGlobal) PasteHdropToFolder(hwnd, site, folder, (HDROP)st.hGlobal, notifyPidl);
    ReleaseStgMedium(&st);
}

static void PasteClipboardToFolder(HWND hwnd, PCWSTR site, PCWSTR folder, PIDLIST_ABSOLUTE notifyPidl)
{
    ProbeLog(L"[PASTE] clipboard-menu-paste site='%s' folder='%s'", site ? site : L"", folder ? folder : L"/");
    if (!OpenClipboard(hwnd)) { ProbeLog(L"[PASTE] OpenClipboard FAILED"); return; }
    HANDLE h = GetClipboardData(CF_HDROP);
    ProbeLog(L"[PASTE] clipboard CF_HDROP=%p", h);
    if (h) PasteHdropToFolder(hwnd, site, folder, (HDROP)h, notifyPidl);
    CloseClipboard();
}
static void BgCustomCommand(HWND hwnd, PCWSTR site, PCWSTR folder, int idx)
{
    CUSTCMD cmds[MAX_CUSTOM] = {};
    int n = LoadCustomCommands(cmds, MAX_CUSTOM);
    if (idx < 0 || idx >= n) return;
    CUSTCMD &c = cmds[idx];
    WCHAR cmd[2400] = {};
    {
        std::wstring s = c.command;
        auto rep = [&](PCWSTR t, PCWSTR v) {
            std::wstring tt = t; size_t at = 0;
            while ((at = s.find(tt, at)) != std::wstring::npos) { s.replace(at, tt.size(), v); at += wcslen(v); }
        };
        rep(L"{site}", site); rep(L"{path}", folder); rep(L"{name}", L""); rep(L"{full}", L"");
        StringCchCopyW(cmd, ARRAYSIZE(cmd), s.c_str());
    }
    if (0 == StrCmpIW(c.type, L"script"))
    {
        WCHAR ws[2600];
        StringCchPrintf(ws, ARRAYSIZE(ws), L"\"%s\" /command \"open \\\"%s\\\"\" \"%s\" \"close\" \"exit\"", GetWinScpPath(), site, cmd);
        STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
        if (CreateProcessW(NULL, ws, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    }
    else
    {
        HINSTANCE hr = ShellExecuteW(hwnd, NULL, cmd, NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)hr <= 32) MessageBoxW(hwnd, ExplorerText(L"error.custom_command_failed", L"自定义命令执行失败。", L"Custom command failed."), ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK|MB_ICONERROR);
    }
}

// ---- 「在此打开终端」 ---------------------------------------------------------
// 形态：wt.exe  "C:\Windows\System32\OpenSSH\ssh.exe" -p <port> <user>@<host> -t "cd '<远端目录>' && exec $SHELL -l"
// 认证交给终端里的 ssh：密码在终端内交互输入，或走 ssh-agent/默认密钥。
// 我们的进程**完全不接触凭据**——这是这条路线的最大收益，也绕开了
// "OpenSSH 不接受命令行密码（密码从 TTY 读）"这个死结。
// 实测依据见 docs/OPEN_IN_TERMINAL_FEASIBILITY.md：
//   * `wt new-tab -p <profile> --appendCommandLine …` 在 WT 1.24.11911 上**不生效**；
//   * `wt <整条 commandline>`（位置参数形式）**生效** → 这里走后者。
static BOOL SiteIsSshCapable(PCWSTR type)
{
    return type && (0 == StrCmpIW(type, L"sftp") || 0 == StrCmpIW(type, L"scp"));
}

static BOOL FindSiteByName(PCWSTR name, FTPSITE *out)
{
    if (!name || !name[0] || !out) return FALSE;
    FTPSITE sites[64] = {};
    int n = FtpSitesGet(sites, ARRAYSIZE(sites));
    for (int k = 0; k < n; k++)
        if (0 == StrCmpIW(sites[k].name, name)) { *out = sites[k]; return TRUE; }
    return FALSE;
}

// POSIX 单引号转义。路径来自服务端目录列表：若含 ' 或 $( ) 而不转义，
// 就等于允许远端文件系统在用户机器上执行任意命令——这是安全问题，不是格式问题。
static void AppendPosixSingleQuoted(std::wstring &out, PCWSTR path)
{
    out += L'\'';
    for (PCWSTR p = path; p && *p; ++p)
    {
        if (*p == L'\'') out += L"'\\''";      // '  变成  '\''
        else out += *p;
    }
    out += L'\'';
}

// Windows 命令行层面的参数引号：ssh 收到的 -t 必须恰好是**一个** argv。
// 规则（CommandLineToArgvW / MSVCRT）：只有紧跟在 " 之前的反斜杠、以及参数末尾的反斜杠
// 才需要加倍，引号本身写成 \"。天真的"所有反斜杠都加倍"会**改写内容**——
// 实测 `\sub` 到远端变成 `\\sub`（见 C:\temp\erf-term-escape-test.ps1 的 round-trip 探针）。
static void AppendWinQuotedArg(std::wstring &out, PCWSTR arg)
{
    out += L'"';
    size_t pendingBackslashes = 0;
    for (PCWSTR p = arg; p && *p; ++p)
    {
        if (*p == L'\\') { pendingBackslashes++; continue; }
        if (*p == L'"')
        {
            out.append(pendingBackslashes * 2 + 1, L'\\');   // 反斜杠加倍 + 转义引号
            out += L'"';
        }
        else
        {
            out.append(pendingBackslashes, L'\\');           // 不紧邻引号：原样
            out += *p;
        }
        pendingBackslashes = 0;
    }
    out.append(pendingBackslashes * 2, L'\\');               // 结尾反斜杠要加倍，否则会吃掉收尾引号
    out += L'"';
}

// 终端种类
enum { TERM_WT = 0, TERM_PWSH = 1, TERM_VSCODE = 2 };

static BOOL EnsureDirectoryExists(PCWSTR path)
{
    if (CreateDirectoryW(path, NULL)) return TRUE;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

// PowerShell 单引号字符串转义：' -> ''（PS 单引号内 $ 不展开，正好把远端命令原样传下去）
static void AppendPsSingleQuoted(std::wstring &out, PCWSTR s)
{
    out += L'\'';
    for (PCWSTR p = s; p && *p; ++p)
    {
        if (*p == L'\'') out += L"''";
        else out += *p;
    }
    out += L'\'';
}

// ── 受管 SSH 别名：判断"这个别名是不是我们自己写的" ───────────────────────
// 我们把站点信息写进 %USERPROFILE%\.ssh\config 的受管块（见 EnsureSshConfigAlias），
// 别名由站点名**确定性**推出，所以不用读配置文件就能认出它属于我们。
//
// 为什么必须区分（2026-09-18 用户实测的坑）：
//   客户端把 .ssh/config 里与站点匹配的 Host 列出来让用户绑定，而我们自己生成的
//   受管块恰好满足"主机/端口/用户一致"，于是用户"绑定"的其实是我们上次生成的块
//   （站点里存下 SshHostAlias=erf-<站点>）。此后每次开终端都直接用这个别名、
//   再不刷新，块里那条 IdentityFile 一旦指向被删掉的私钥，就永远报
//   `no such identity: …\ssh\erf_ed25519: No such file or directory` 然后莫名退回密码登录。
static std::wstring ManagedAliasForSite(const FTPSITE &site)
{
    std::wstring safe;
    for (PCWSTR p = site.name; p && *p; ++p)
        safe += (iswalnum((wint_t)*p) || *p == L'-' || *p == L'_' || *p == L'.') ? *p : L'-';
    if (safe.empty()) safe = L"site";
    return L"erf-" + safe;
}

static BOOL EnsureSshConfigAlias(const FTPSITE &site, std::wstring &alias);   // 定义在下面
static BOOL FindUserSshAliasForSite(const FTPSITE &site, std::wstring &alias); // 定义在下面

// 别名是"我们自己的受管块"（或站点还没绑定别名）时，按当前站点信息把块重写一遍。
// 用户的 Host 一律不碰。
static void RefreshManagedAliasIfOurs(const FTPSITE &site)
{
    if (site.sshAlias[0] && _wcsicmp(site.sshAlias, ManagedAliasForSite(site).c_str()) != 0)
        return;                                    // 用户自己绑的 Host：原样使用
    std::wstring alias;
    EnsureSshConfigAlias(site, alias);
}

// 把真正的 ssh 调用写进脚本文件，终端只负责"跑这个脚本"。
//
// 为什么不直接把 ssh 命令塞给 wt：Windows Terminal 有自己的命令行语法
// （`;` 是它的命令分隔符，`&&`、引号的处理也不遵循 MSVCRT 规则），实测会把复杂参数切碎，
// 再把碎片当成"另一条命令"去启动，用户看到
// 「错误 2147942402 (0x80070002) 启动"…"时 系统找不到指定的文件」。
// 绕开它的唯一可靠办法：命令行里只留一个简单路径。
//
// ⚠ 必须写成 UTF-8 **带 BOM**：powershell.exe 5.1 对无 BOM 的 .ps1 按 ANSI 解析，
//   目录名里的中文会直接解析失败（今天已在另一个脚本上踩过这个坑）。
static BOOL WriteTerminalShim(const FTPSITE &site, PCWSTR remoteDir,
                              std::wstring &shimPath, std::wstring &sshCmdText)
{
    // 站点绑的是我们自己的受管别名（或还没绑定）时，先把 .ssh/config 里那一块按当前站点
    // 信息重写一遍：否则一条指向"已被删掉的私钥"的 IdentityFile 会永远留在那里，
    // 每次开终端都报 no such identity，然后莫名退回密码登录。
    RefreshManagedAliasIfOurs(site);

    WCHAR local[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local))) return FALSE;
    std::wstring base = std::wstring(local) + L"\\ExplorerRemoteFs";
    std::wstring dir = base + L"\\term";
    if (!EnsureDirectoryExists(base.c_str()) || !EnsureDirectoryExists(dir.c_str())) return FALSE;

    std::wstring safe;
    for (PCWSTR p = site.name; p && *p; ++p)
        safe += (iswalnum((wint_t)*p) || *p == L'-' || *p == L'_' || *p == L'.') ? *p : L'-';
    if (safe.empty()) safe = L"site";
    shimPath = dir + L"\\erf-" + safe + L".ps1";

    // ssh 用 System32 的 OpenSSH：PATH 里 Git 的 MSYS ssh 排在前面，行为不同。
    WCHAR sshExe[MAX_PATH] = L"C:\\Windows\\System32\\OpenSSH\\ssh.exe";
    if (GetFileAttributesW(sshExe) == INVALID_FILE_ATTRIBUTES)
        StringCchCopyW(sshExe, ARRAYSIZE(sshExe), L"ssh.exe");

    // 远端命令：cd 失败也要给 shell（否则窗口一闪就关）。
    // 这里的 POSIX 单引号转义是安全边界：目录名里的 ' 若不被转义，等于允许对方执行任意命令。
    PCWSTR dirForCmd = (remoteDir && remoteDir[0]) ? remoteDir : L"/";
    std::wstring remote = L"cd ";
    AppendPosixSingleQuoted(remote, dirForCmd);
    remote += L" || printf '[ERF] cannot enter: %s\\n' ";
    AppendPosixSingleQuoted(remote, dirForCmd);
    remote += L" && exec ${SHELL:=/bin/sh} -l";

    WCHAR portText[16] = {};
    StringCchPrintfW(portText, ARRAYSIZE(portText), L"%d", site.port ? site.port : 22);
    std::wstring target = std::wstring(site.user) + L"@" + site.host;

    // 认证交给 ssh：站点绑定了现有 SSH 配置就直接用别名（用户的密钥/agent/端口全部照旧），
    // 否则用站点的端口 + PrivateKeyPath，再退到 ssh-agent / 默认密钥 / 终端内输密码。
    // 我们的进程始终不接触凭据。
    // accept-new：首连自动接受主机密钥（TOFU），避免用户被指纹交互卡住。
    sshCmdText = L"& ";
    AppendPsSingleQuoted(sshCmdText, sshExe);
    sshCmdText += L" -o StrictHostKeyChecking=accept-new";
    std::wstring userAlias;
    if (site.sshAlias[0])
    {
        sshCmdText += L" ";
        AppendPsSingleQuoted(sshCmdText, site.sshAlias);
    }
    else if (FindUserSshAliasForSite(site, userAlias))
    {
        // 站点没绑定别名，但用户自己的 ~/.ssh/config 里有一条"同一台机器、同一个用户"的 Host
        // → 直接复用它（密钥、ssh-agent、known_hosts 全部照旧），而不是退回
        // "端口 + 私钥文件 + 交互式密码"。这正是用户要的"复用已有的 SSH 配置"。
        sshCmdText += L" ";
        AppendPsSingleQuoted(sshCmdText, userAlias.c_str());
    }
    else
    {
        sshCmdText += L" -p ";
        sshCmdText += portText;
        if (site.keyPath[0])
        {
            // 私钥文件可能早就不在了（换机器、清理过 %LOCALAPPDATA%、手工删过）。
            // 直接把它交给 ssh 只会得到
            //   "no such identity: …\ssh\erf_ed25519: No such file or directory"
            // 然后莫名其妙地退回密码登录 —— 所以先确认文件真的在，不在就当没配过，
            // 让 ssh 走 agent / 默认密钥 / 交互式密码。
            if (GetFileAttributesW(site.keyPath) == INVALID_FILE_ATTRIBUTES)
                ProbeLog(L"[TERM] PrivateKeyPath missing, ignored: '%s'", site.keyPath);
            else
            {
                sshCmdText += L" -i ";
                AppendPsSingleQuoted(sshCmdText, site.keyPath);
            }
        }
        sshCmdText += L" ";
        AppendPsSingleQuoted(sshCmdText, target.c_str());
    }
    sshCmdText += L" -t ";
    AppendPsSingleQuoted(sshCmdText, remote.c_str());

    std::wstring script =
        L"# Generated by ERF (ExploreRemoteFiles); regenerated on every use. Safe to delete.\r\n"
        L"$ErrorActionPreference = 'Continue'\r\n" + sshCmdText + L"\r\n";

    HANDLE h = CreateFileW(shimPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    const BYTE bom[3] = { 0xEF, 0xBB, 0xBF };
    DWORD written = 0;
    int bytes = WideCharToMultiByte(CP_UTF8, 0, script.c_str(), (int)script.size(), NULL, 0, NULL, NULL);
    std::string utf8((size_t)(bytes > 0 ? bytes : 0), '\0');
    if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, script.c_str(), (int)script.size(), &utf8[0], bytes, NULL, NULL);
    BOOL ok = WriteFile(h, bom, sizeof(bom), &written, NULL);
    if (ok) ok = WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, NULL);
    CloseHandle(h);
    return ok ? TRUE : FALSE;
}

// 在 %USERPROFILE%\.ssh\config 里维护一个受管块（幂等：先删旧块再追加）。
// 目的：让 VS Code Remote-SSH 与命令行 ssh 复用同一套 主机/端口/用户/密钥 ——
// 这正是"优先复用 SSH 已有验证"的落点：密钥、agent、known_hosts 全部照旧生效。
// 只动我们自己标记之间的内容，其余配置原样保留。
static BOOL EnsureSshConfigAlias(const FTPSITE &site, std::wstring &alias)
{
    alias = ManagedAliasForSite(site);

    WCHAR profile[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"USERPROFILE", profile, ARRAYSIZE(profile))) return FALSE;
    std::wstring sshDir = std::wstring(profile) + L"\\.ssh";
    std::wstring cfg = sshDir + L"\\config";
    EnsureDirectoryExists(sshDir.c_str());

    std::string existing;
    HANDLE h = CreateFileW(cfg.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD size = GetFileSize(h, NULL);
        if (size > 0 && size < 4u * 1024 * 1024)
        {
            existing.resize(size);
            DWORD got = 0;
            ReadFile(h, &existing[0], size, &got, NULL);
            existing.resize(got);
        }
        CloseHandle(h);
    }

    const std::string begin = "# >>> ExploreRemoteFiles managed block >>>";
    const std::string end = "# <<< ExploreRemoteFiles managed block <<<";
    size_t b = existing.find(begin);
    if (b != std::string::npos)
    {
        size_t e = existing.find(end, b);
        if (e != std::string::npos) existing.erase(b, e + end.size() - b);
    }

    WCHAR portText[16] = {};
    StringCchPrintfW(portText, ARRAYSIZE(portText), L"%d", site.port ? site.port : 22);
    std::wstring block = L"\r\n" + std::wstring(begin.begin(), begin.end()) + L"\r\n";
    block += L"Host " + alias + L"\r\n";
    block += L"    HostName " + std::wstring(site.host) + L"\r\n";
    block += L"    Port " + std::wstring(portText) + L"\r\n";
    block += L"    User " + std::wstring(site.user) + L"\r\n";
    // 私钥文件不存在就**不要**写这一行：ssh 读到不存在的 IdentityFile 会报
    // "no such identity: …（No such file or directory）" 然后再退回交互密码，
    // 比不写更糟 —— 用户以为已经配好免密了。不写这一行，ssh 会正常走
    // ssh-agent → 默认密钥（~/.ssh/id_*）→ 交互式密码。
    if (site.keyPath[0] && GetFileAttributesW(site.keyPath) != INVALID_FILE_ATTRIBUTES)
        block += L"    IdentityFile " + std::wstring(site.keyPath) + L"\r\n";
    else if (site.keyPath[0])
        ProbeLog(L"[TERM] ssh config: PrivateKeyPath missing, IdentityFile omitted: '%s'", site.keyPath);
    block += std::wstring(end.begin(), end.end()) + L"\r\n";

    std::string out = existing;
    int bytes = WideCharToMultiByte(CP_UTF8, 0, block.c_str(), (int)block.size(), NULL, 0, NULL, NULL);
    if (bytes > 0)
    {
        std::string u((size_t)bytes, '\0');
        WideCharToMultiByte(CP_UTF8, 0, block.c_str(), (int)block.size(), &u[0], bytes, NULL, NULL);
        out += u;
    }

    h = CreateFileW(cfg.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD written = 0;
    BOOL ok = WriteFile(h, out.data(), (DWORD)out.size(), &written, NULL);
    CloseHandle(h);
    return ok;
}

// 从 Code.exe 的完整路径推出 CLI 包装脚本 <安装目录>\bin\code.cmd。
// 我们要的是 code.cmd：只有它会把参数转发给正在运行的 VS Code 实例。
static BOOL DeriveCodeCmdFromExe(PCWSTR codeExe, std::wstring &out)
{
    if (!codeExe || !codeExe[0]) return FALSE;
    std::wstring exe = codeExe;
    if (exe.size() >= 2 && exe.front() == L'"' && exe.back() == L'"') exe = exe.substr(1, exe.size() - 2);
    size_t slash = exe.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return FALSE;
    std::wstring cmd = exe.substr(0, slash) + L"\\bin\\code.cmd";
    if (GetFileAttributesW(cmd.c_str()) == INVALID_FILE_ATTRIBUTES) return FALSE;
    out = cmd;
    return TRUE;
}

// 解析 code.cmd 的真实位置。
// 旧代码只查 %LOCALAPPDATA%\Programs（用户级安装），装在 D:\Program\Microsoft VS Code
// 这类自定义/系统级目录时就会报"未找到 code.cmd"。
// 顺序：注册表 App Paths（HKCU→HKLM）→ Uninstall 的 InstallLocation → 常见路径 → PATH。
static BOOL ResolveCodeCmd(std::wstring &out)
{
    const HKEY roots[2] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };

    // 1) App Paths\code.exe （默认值指向 Code.exe）
    for (int i = 0; i < 2; ++i)
    {
        HKEY k = NULL;
        if (RegOpenKeyExW(roots[i], L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\code.exe",
                          0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS)
        {
            WCHAR val[MAX_PATH * 2] = {};
            DWORD cb = sizeof(val), type = 0;
            LONG rc = RegQueryValueExW(k, NULL, NULL, &type, (LPBYTE)val, &cb);
            RegCloseKey(k);
            if (rc == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && val[0])
            {
                if (type == REG_EXPAND_SZ)
                {
                    WCHAR expanded[MAX_PATH * 2] = {};
                    ExpandEnvironmentStringsW(val, expanded, ARRAYSIZE(expanded));
                    StringCchCopyW(val, ARRAYSIZE(val), expanded);
                }
                if (DeriveCodeCmdFromExe(val, out)) return TRUE;
            }
        }
    }

    // 2) 卸载信息里的 InstallLocation（自定义安装目录常在这里）
    for (int i = 0; i < 2; ++i)
    {
        HKEY uninst = NULL;
        if (RegOpenKeyExW(roots[i], L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                          0, KEY_ENUMERATE_SUB_KEYS, &uninst) != ERROR_SUCCESS) continue;
        WCHAR sub[256] = {}; DWORD index = 0, cch = ARRAYSIZE(sub);
        while (RegEnumKeyExW(uninst, index++, sub, &cch, NULL, NULL, NULL, NULL) == ERROR_SUCCESS)
        {
            cch = ARRAYSIZE(sub);
            HKEY app = NULL;
            if (RegOpenKeyExW(uninst, sub, 0, KEY_QUERY_VALUE, &app) == ERROR_SUCCESS)
            {
                WCHAR display[256] = {}, loc[MAX_PATH * 2] = {};
                DWORD cb = sizeof(display), type = 0;
                if (RegQueryValueExW(app, L"DisplayName", NULL, &type, (LPBYTE)display, &cb) == ERROR_SUCCESS
                    && (0 == StrCmpIW(display, L"Microsoft VS Code") || 0 == StrCmpIW(display, L"Microsoft VS Code Insiders")))
                {
                    cb = sizeof(loc);
                    if (RegQueryValueExW(app, L"InstallLocation", NULL, &type, (LPBYTE)loc, &cb) == ERROR_SUCCESS && loc[0])
                    {
                        std::wstring cmd = std::wstring(loc) + L"\\bin\\code.cmd";
                        if (GetFileAttributesW(cmd.c_str()) != INVALID_FILE_ATTRIBUTES)
                        {
                            RegCloseKey(app); RegCloseKey(uninst);
                            out = cmd; return TRUE;
                        }
                    }
                }
                RegCloseKey(app);
            }
        }
        RegCloseKey(uninst);
    }

    // 3) 常见安装位置（用户级 / 系统级 / Insiders）
    WCHAR local[MAX_PATH] = {}, pf[MAX_PATH] = {}, pf86[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local));
    GetEnvironmentVariableW(L"ProgramFiles", pf, ARRAYSIZE(pf));
    GetEnvironmentVariableW(L"ProgramFiles(x86)", pf86, ARRAYSIZE(pf86));
    const wchar_t *userTails[] = {
        L"\\Programs\\Microsoft VS Code\\bin\\code.cmd",
        L"\\Programs\\Microsoft VS Code Insiders\\bin\\code-insiders.cmd",
    };
    for (int i = 0; i < ARRAYSIZE(userTails); ++i)
    {
        std::wstring p = std::wstring(local) + userTails[i];
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) { out = p; return TRUE; }
    }
    const wchar_t *machineTails[] = {
        L"\\Microsoft VS Code\\bin\\code.cmd",
        L"\\Microsoft VS Code Insiders\\bin\\code-insiders.cmd",
    };
    const std::wstring bases[2] = { pf, pf86 };
    for (int i = 0; i < 2; ++i)
    {
        if (bases[i].empty()) continue;
        for (int j = 0; j < ARRAYSIZE(machineTails); ++j)
        {
            std::wstring p = bases[i] + machineTails[j];
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) { out = p; return TRUE; }
        }
    }

    // 4) PATH
    const wchar_t *names[2] = { L"code.cmd", L"code-insiders.cmd" };
    for (int i = 0; i < 2; ++i)
    {
        WCHAR found[MAX_PATH] = {};
        if (SearchPathW(NULL, names[i], NULL, ARRAYSIZE(found), found, NULL))
        {
            out = found;
            return TRUE;
        }
    }
    return FALSE;
}

// 把「在此打开终端」的目标写进请求文件，交给 VS Code 里的 ERF 伴随扩展处理：
//   已有窗口连着该 authority → 直接在那个窗口里新开终端（不新开窗口）
//   没有窗口 → 我们随后启动窗口，扩展激活时接单，终端仍落在目标目录
// 协议见 src-vscode-extension/ （authority / path / ts，TTL 120s，消费后删除）。
static BOOL WriteVscodeRequest(PCWSTR alias, PCWSTR remoteDir)
{
    if (!alias || !alias[0]) return FALSE;
    WCHAR local[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local))) return FALSE;
    std::wstring dir = std::wstring(local) + L"\\ExplorerRemoteFs";
    if (!EnsureDirectoryExists(dir.c_str())) return FALSE;
    std::wstring file = dir + L"\\vscode-request.json";

    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    unsigned long long ms = (u.QuadPart - 116444736000000000ULL) / 10000ULL;

    std::wstring json = L"{\"authority\":\"ssh-remote+";
    for (PCWSTR p = alias; *p; ++p)                    // 别名按白名单过滤后落地
        if (iswalnum((wint_t)*p) || *p == L'-' || *p == L'_' || *p == L'.') json += *p;
    json += L"\",\"path\":\"";
    for (PCWSTR p = (remoteDir && remoteDir[0]) ? remoteDir : L"/"; *p; ++p)
    {
        if (*p == L'"' || *p == L'\\') json += L'\\';
        json += *p;
    }
    json += L"\",\"ts\":";
    WCHAR ts[32] = {};
    StringCchPrintfW(ts, ARRAYSIZE(ts), L"%llu", ms);
    json += ts;
    json += L"}";

    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        ProbeLog(L"[TERM] vscode request write failed err=%lu", GetLastError());
        return FALSE;
    }
    int bytes = WideCharToMultiByte(CP_UTF8, 0, json.c_str(), (int)json.size(), NULL, 0, NULL, NULL);
    std::string utf8((size_t)(bytes > 0 ? bytes : 0), '\0');
    if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, json.c_str(), (int)json.size(), &utf8[0], bytes, NULL, NULL);
    DWORD written = 0;
    BOOL ok = WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, NULL);
    CloseHandle(h);
    ProbeLog(L"[TERM] vscode request file='%s' alias='%s' dir='%s' ok=%d", file.c_str(), alias, remoteDir, ok);
    return ok;
}


// 异步的「递归修改权限」。属性页按下确定后立即返回，遍历与进度由常驻服务负责
// （进度窗口带「取消」）；这里只在线程结束时把新权限刷进视图。
// 与同步版的关键差别：不再有 30 秒被杀掉的风险，Explorer 也不会卡住。
struct ChmodRemoteCtx
{
    WCHAR site[64];
    WCHAR path[600];
    WCHAR parent[600];
    WCHAR mode[8];
};

static DWORD WINAPI ChmodRemoteThreadProc(LPVOID p)
{
    ChmodRemoteCtx *c = static_cast<ChmodRemoteCtx *>(p);
    ProbeLog(L"[TERM] async chmod begin site='%s' path='%s' mode=%s", c->site, c->path, c->mode);

    std::string reply;
    BOOL ok = FtpBridgeChmod(c->site, c->path, c->mode, TRUE, reply);
    BOOL serviceUnavailable = (!ok && reply.empty());     // 管道都连不上：常驻服务没运行
    if (!ok && serviceUnavailable)
    {
        // 回退：直接跑 CLI（这里在工作线程上，长超时是安全的）。
        // 不能让"常驻服务没开"变成静默失败——用户必须知道权限到底改没改。
        ProbeLog(L"[TERM] async chmod: resident service unavailable, falling back to CLI");
        int rc = RunCli(c->site, L"chmodr", c->path, c->mode, NULL, 60u * 60u * 1000u);
        ok = (rc == 0);
        if (ok) { AfterRemoteMutation(c->site, c->parent, NULL); }
        else
        {
            ProbeLog(L"[TERM] async chmod CLI fallback failed rc=%d", rc);
            MessageBoxW(NULL,
                ExplorerText(L"error.chmod_failed", L"权限修改失败（常驻服务未运行，已回退到命令行）。",
                                                      L"Permission update failed (resident service was not running; fell back to the CLI)."),
                ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK | MB_ICONERROR);
        }
        delete c;
        DllRelease();
        return 0;
    }
    if (ok)
    {
        // 权限真的改完了，才刷新视图（与服务端状态保持一致）。
        AfterRemoteMutation(c->site, c->parent, NULL);
        ProbeLog(L"[TERM] async chmod done site='%s' path='%s'", c->site, c->path);
    }
    else
    {
        // 失败或被用户在进度窗口里取消：那个窗口已经给出了结果与原因，
        // 这里不再弹第二个窗（重复告警），只留日志。
        ProbeLog(L"[TERM] async chmod failed site='%s' path='%s' reply='%hs'", c->site, c->path, reply.c_str());
    }

    delete c;
    DllRelease();
    return 0;
}

static void StartChmodRecursiveAsync(PCWSTR site, PCWSTR path, PCWSTR modeOctal)
{
    if (!site || !site[0] || !path || !path[0]) return;
    ChmodRemoteCtx *c = new (std::nothrow) ChmodRemoteCtx();
    if (!c) return;
    StringCchCopyW(c->site, ARRAYSIZE(c->site), site);
    StringCchCopyW(c->path, ARRAYSIZE(c->path), path);
    StringCchCopyW(c->mode, ARRAYSIZE(c->mode), modeOctal ? modeOctal : L"644");
    PathParent(c->path, c->parent, ARRAYSIZE(c->parent));

    DllAddRef();   // 2026-09-20: pin the module until the chmod worker returns
    HANDLE t = CreateThread(NULL, 0, ChmodRemoteThreadProc, c, 0, NULL);
    if (t) CloseHandle(t);
    else { DllRelease(); delete c; ProbeLog(L"[TERM] async chmod: CreateThread failed err=%lu", GetLastError()); }
}

// ── 把 VS Code 窗口请到前台 ────────────────────────────────────────────────
// 用户要求：在已有窗口里开完终端后，应当"跳转"到那个窗口，而不是让终端在后台出现。
// 扩展 API 没有操作系统级聚焦能力（terminal.show() 只作用于窗口内部），所以这一步
// 由我们这边做：找标题里带 "[SSH: <别名>]" 的 VS Code 顶层窗口（Chrome_WidgetWin_1），
// 还原并 SetForegroundWindow。我们的代码跑在 explorer.exe 里，点击时它就是前台进程，
// 因此 SetForegroundWindow 不会被前台锁挡住。
struct TerminalFocusRequest
{
    std::wstring alias;
    DWORD timeoutMs;
};

// 后台线程：窗口可能刚启动还没出现，最多等 timeoutMs；找不到就静默放弃
// （聚焦只是体验优化，失败不能影响终端已经开出来这个结果）。
static DWORD WINAPI TerminalFocusThread(LPVOID param)
{
    TerminalFocusRequest *req = reinterpret_cast<TerminalFocusRequest *>(param);
    if (!req) { DllRelease(); return 0; }
    const DWORD step = 250;
    for (DWORD waited = 0; waited <= req->timeoutMs; waited += step)
    {
        HWND wnd = ErfFindVscodeWindowForAlias(req->alias.c_str());
        if (wnd)
        {
            if (IsIconic(wnd)) ShowWindow(wnd, SW_RESTORE);
            SetForegroundWindow(wnd);
            ProbeLog(L"[TERM] focused vscode window hwnd=%p alias='%s'", wnd, req->alias.c_str());
            break;
        }
        Sleep(step);
    }
    delete req;
    DllRelease();
    return 0;
}

static void FocusVscodeWindowAsync(PCWSTR alias, DWORD timeoutMs)
{
    if (!alias || !alias[0]) return;
    TerminalFocusRequest *req = new (std::nothrow) TerminalFocusRequest();
    if (!req) return;
    req->alias = alias;
    req->timeoutMs = timeoutMs;
    DllAddRef();   // 2026-09-20: pin the module until the focus worker returns
    HANDLE t = CreateThread(NULL, 0, TerminalFocusThread, req, 0, NULL);
    if (t) CloseHandle(t); else { DllRelease(); delete req; }
}
// ── Windows Terminal：用 fragment 定义我们的 profile（不改用户的 settings.json）──
// WT 会扫描 %LOCALAPPDATA%\Microsoft\Windows Terminal\Fragments\<App>\*.json，
// 把它当作额外的 profile 来源（VS Code、Azure 等就是这么做的）。于是：
//   * 一个站点一个文件，幂等（覆盖自己的文件），删除即撤销；
//   * 完全不动用户的 settings.json —— 之前把 profile 直接写进去，等于改用户的配置，
//     而且 WT 会重写整个文件（实测它把我们的条目重排过）；
//   * 启动只给 `wt -p "ERF: <站点>"`，WT 自己的命令行解析器无从插手。
static void SafeToken(PCWSTR src, std::wstring &out)
{
    out.clear();
    for (PCWSTR p = src; p && *p; ++p)
        out += (iswalnum((wint_t)*p) || *p == L'-' || *p == L'_' || *p == L'.') ? *p : L'-';
    if (out.empty()) out = L"site";
}

// 由站点名推出稳定的 GUID（MD5 前 16 字节），保证同一站点每次得到同一个 id。
static void DeterministicGuid(PCWSTR name, WCHAR *out, UINT cch)
{
    BYTE hash[16] = {};
    HCRYPTPROV prov = 0;
    HCRYPTHASH h = 0;
    if (CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
    {
        if (CryptCreateHash(prov, CALG_MD5, 0, 0, &h))
        {
            int bytes = (int)(wcslen(name) * sizeof(WCHAR));
            CryptHashData(h, (const BYTE *)name, (DWORD)bytes, 0);
            DWORD got = 16;
            CryptGetHashParam(h, HP_HASHVAL, hash, &got, 0);
            CryptDestroyHash(h);
        }
        CryptReleaseContext(prov, 0);
    }
    // 版本位按 UUIDv4 风格摆一下，纯粹为了看起来像个合法 GUID
    hash[6] = (BYTE)((hash[6] & 0x0F) | 0x40);
    hash[8] = (BYTE)((hash[8] & 0x3F) | 0x80);
    StringCchPrintfW(out, cch,
        L"{%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
        hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7],
        hash[8], hash[9], hash[10], hash[11], hash[12], hash[13], hash[14], hash[15]);
}

// ── 读用户自己的 ~/.ssh/config，找可以复用的 Host ─────────────────────────
// 与 C# 侧 SshConfigReader 的语义必须一致（两边都保守）：
//   · 跳过我们自己的受管块（那不是"用户已有的配置"，把它列出来就是让用户绑定到自己）
//   · 主机名 / 用户名 / 端口三项全等才算匹配 —— 否则会出现"在本站点右键却登进另一台机器"
//   · 带通配符的 Host、Match 块、Include 一律不展开
struct SshConfigHost
{
    std::wstring alias;
    std::wstring hostName;   // 没写 HostName 时，按 ssh 语义 = 别名本身
    std::wstring user;
    int port = 22;
    bool managed = false;    // 落在我们的受管块里
};

static void ParseUserSshConfig(std::vector<SshConfigHost> &out)
{
    out.clear();
    WCHAR profile[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"USERPROFILE", profile, ARRAYSIZE(profile))) return;
    std::wstring cfg = std::wstring(profile) + L"\\.ssh\\config";
    HANDLE h = CreateFileW(cfg.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string raw;
    DWORD size = GetFileSize(h, NULL);
    if (size > 0 && size < 4u * 1024 * 1024)
    {
        raw.resize(size); DWORD got = 0;
        if (ReadFile(h, &raw[0], size, &got, NULL)) raw.resize(got); else raw.clear();
    }
    CloseHandle(h);
    if (raw.empty()) return;

    std::wstring text;
    int wide = MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), NULL, 0);
    if (wide > 0) { text.resize((size_t)wide); MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), &text[0], wide); }

    const std::wstring begin = L"# >>> ExploreRemoteFiles managed block >>>";
    const std::wstring end   = L"# <<< ExploreRemoteFiles managed block <<<";
    bool managed = false, inMatch = false, hasBlock = false;
    SshConfigHost cur;
    std::vector<std::wstring> names;

    auto flush = [&]()
    {
        if (!names.empty())
        {
            std::wstring alias;
            for (auto const &n : names)
                if (n.find(L'*') == std::wstring::npos && n.find(L'?') == std::wstring::npos && n[0] != L'!')
                { alias = n; break; }
            if (!alias.empty())
            {
                SshConfigHost e = cur;
                e.alias = alias;
                if (e.hostName.empty()) e.hostName = alias;
                e.managed = managed;
                out.push_back(e);
            }
        }
        names.clear(); cur = SshConfigHost(); hasBlock = false;
    };

    size_t pos = 0;
    while (pos <= text.size())
    {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, (nl == std::wstring::npos ? text.size() : nl) - pos);
        pos = (nl == std::wstring::npos) ? text.size() + 1 : nl + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' || line.back() == L'\t')) line.pop_back();
        size_t st = 0; while (st < line.size() && (line[st] == L' ' || line[st] == L'\t')) ++st;
        line = line.substr(st);

        if (line == begin) { flush(); managed = true; continue; }
        if (line == end)   { flush(); managed = false; continue; }
        if (managed || line.empty() || line[0] == L'#') continue;

        size_t sep = line.find_first_of(L" \t=");
        std::wstring key = (sep == std::wstring::npos) ? line : line.substr(0, sep);
        std::wstring val = (sep == std::wstring::npos) ? L"" : line.substr(sep + 1);
        while (!val.empty() && (val.front() == L' ' || val.front() == L'\t' || val.front() == L'=')) val.erase(val.begin());

        if (0 == _wcsicmp(key.c_str(), L"Host"))
        {
            flush(); inMatch = false; hasBlock = true;
            size_t p2 = 0;
            while (p2 < val.size())
            {
                size_t sp = val.find_first_of(L" \t", p2);
                std::wstring n = val.substr(p2, (sp == std::wstring::npos ? val.size() : sp) - p2);
                if (!n.empty()) names.push_back(n);
                if (sp == std::wstring::npos) break;
                p2 = sp + 1;
            }
        }
        else if (0 == _wcsicmp(key.c_str(), L"Match")) { flush(); inMatch = true; }
        else if (0 == _wcsicmp(key.c_str(), L"Include")) { /* 不展开：安全优先 */ }
        else if (!inMatch && hasBlock)
        {
            if (0 == _wcsicmp(key.c_str(), L"HostName")) cur.hostName = val;
            else if (0 == _wcsicmp(key.c_str(), L"User")) cur.user = val;
            else if (0 == _wcsicmp(key.c_str(), L"Port")) { int p3 = _wtoi(val.c_str()); if (p3 > 0) cur.port = p3; }
        }
    }
    flush();
}

// 站点没绑定别名时，先在这里找一条用户自己的 Host：找到就直接复用（主机/端口/用户全等），
// 找不到才由我们生成受管块。这样"右键打开终端"优先用用户已有的密钥/agent/known_hosts。
static BOOL FindUserSshAliasForSite(const FTPSITE &site, std::wstring &alias)
{
    std::vector<SshConfigHost> hosts;
    ParseUserSshConfig(hosts);
    int sitePort = site.port ? site.port : 22;
    for (auto const &h : hosts)
    {
        if (h.managed) continue;
        if (0 != _wcsicmp(h.hostName.c_str(), site.host)) continue;
        if (0 != _wcsicmp(h.user.c_str(), site.user)) continue;
        if (h.port != sitePort) continue;
        alias = h.alias;
        ProbeLog(L"[TERM] reusing user's ssh host '%s' for site '%s'", alias.c_str(), site.name);
        return TRUE;
    }
    return FALSE;
}

// JSON 字符串转义：反斜杠、双引号、控制字符。
// **凡是要放进 JSON 字符串的内容都必须走这里**，包括我们自己拼进去的包裹引号 ——
// 旧代码只在循环里转义路径字符、把两个包裹引号加在循环外，写出的 fragment 不是合法 JSON，
// WT 整份忽略，于是 `wt -p "ERF: <站点>"` 永远找不到 profile（用户看到"回退到本机默认路径"）。
static void AppendJsonEscaped(std::wstring &out, PCWSTR s)
{
    for (PCWSTR p = s; p && *p; ++p)
    {
        switch (*p)
        {
        case L'"':  out += L"\\\""; break;
        case L'\\': out += L"\\\\"; break;
        case L'\n': out += L"\\n";  break;
        case L'\r': out += L"\\r";  break;
        case L'\t': out += L"\\t";  break;
        default:
            if (*p < 0x20)
            {
                WCHAR esc[8] = {};
                StringCchPrintfW(esc, ARRAYSIZE(esc), L"\\u%04X", (UINT)(unsigned short)*p);
                out += esc;
            }
            else
            {
                out += *p;
            }
        }
    }
}

static BOOL WriteTerminalFragment(const FTPSITE &site, PCWSTR shimPath, std::wstring &profileName)
{
    WCHAR local[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local))) return FALSE;

    std::wstring wtRoot = std::wstring(local) + L"\\Microsoft\\Windows Terminal";
    std::wstring dir = wtRoot + L"\\Fragments\\ExploreRemoteFiles";
    if (!EnsureDirectoryExists(wtRoot.c_str())
        || !EnsureDirectoryExists((wtRoot + L"\\Fragments").c_str())
        || !EnsureDirectoryExists(dir.c_str()))
        return FALSE;

    std::wstring safe;
    SafeToken(site.name, safe);
    std::wstring file = dir + L"\\erf-" + safe + L".json";

    WCHAR guid[64] = {};
    DeterministicGuid(site.name, guid, ARRAYSIZE(guid));
    profileName = L"ERF: " + std::wstring(site.name);

    // 先拼"真正要执行的命令行"（**含包裹引号**），再整体做 JSON 转义。
    //
    // 这里踩过一个必现的坑（2026-09-18 用户实测"wt 直接回退到本机默认路径"）：
    // 旧代码把两个包裹引号加在转义循环**之外**，只有路径里的字符被转义，于是写出来是
    //   "commandline": "powershell.exe … -File "C:\\…\\erf-WSL-SFTP.ps1""
    // —— 这根本不是合法 JSON（两个裸引号提前结束了字符串），WT 对 fragment 是严格解析，
    // 整份文件被忽略 ⇒ profile "ERF: <站点>" 不存在 ⇒ `wt -p "ERF: <站点>"` 回退到
    // 默认 profile/默认起始目录。凡是"字符串里的引号"，都必须由转义函数统一处理。
    std::wstring cmdRaw = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"";
    cmdRaw += shimPath;
    cmdRaw += L"\"";

    std::wstring json = L"{\r\n  \"profiles\": [\r\n    {\r\n      \"name\": \"";
    AppendJsonEscaped(json, profileName.c_str());
    json += L"\",\r\n      \"guid\": \"";
    AppendJsonEscaped(json, guid);
    json += L"\",\r\n      \"hidden\": false,\r\n      \"commandline\": \"";
    AppendJsonEscaped(json, cmdRaw.c_str());
    json += L"\"\r\n    }\r\n  ]\r\n}\r\n";

    int bytes = WideCharToMultiByte(CP_UTF8, 0, json.c_str(), (int)json.size(), NULL, 0, NULL, NULL);
    if (bytes <= 0) return FALSE;
    std::string utf8((size_t)bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, json.c_str(), (int)json.size(), &utf8[0], bytes, NULL, NULL);

    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        ProbeLog(L"[TERM] WT fragment write failed err=%lu file='%s'", GetLastError(), file.c_str());
        return FALSE;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, NULL);
    CloseHandle(h);
    ProbeLog(L"[TERM] WT fragment file='%s' profile='%s' guid=%s ok=%d", file.c_str(), profileName.c_str(), guid, ok);
    return ok;
}

static void LaunchTerminalForSite(HWND hwnd, const FTPSITE &site, PCWSTR remoteDir, int which)
{
    if (!SiteIsSshCapable(site.type))
    {
        // FTP 没有 shell 通道：明确告知，而不是给一个永远失败的菜单项。
        MessageBoxW(hwnd,
            ExplorerText(L"error.terminal_needs_ssh",
                         L"该站点不是 SSH 类型（SFTP），FTP 没有 shell 通道，无法打开终端。",
                         L"This site is not SSH-based (SFTP). FTP has no shell channel, so no terminal can be opened."),
            ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::wstring shimPath, sshCmdText;
    if (!WriteTerminalShim(site, remoteDir, shimPath, sshCmdText))
    {
        ProbeLog(L"[TERM] shim write failed err=%lu", GetLastError());
        MessageBoxW(hwnd,
            ExplorerText(L"error.terminal_shim_failed",
                         L"无法写入终端启动脚本（%LOCALAPPDATA% 不可写？）。",
                         L"Could not write the terminal launcher script (is %LOCALAPPDATA% writable?)."),
            ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK | MB_ICONERROR);
        return;
    }

    std::wstring full;
    DWORD flags = 0;

    if (which == TERM_WT)
    {
        WCHAR local[MAX_PATH] = {};
        GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local));
        std::wstring wt = std::wstring(local) + L"\\Microsoft\\WindowsApps\\wt.exe";
        std::wstring profileName;
        if (GetFileAttributesW(wt.c_str()) == INVALID_FILE_ATTRIBUTES || !WriteTerminalFragment(site, shimPath.c_str(), profileName))
        {
            // 没有 Windows Terminal，或 fragment 写不进去 → 退化为 PowerShell 控制台（同一份 shim）
            which = TERM_PWSH;
        }
        else
        {
            // 关键：交给 wt 的**只有** -p 和一个引号包起来的 profile 名。
            // 真正的命令在 fragment 里（由 WT 按正常规则解析），
            // 于是 WT 自己的命令行语法（吃以 - 开头的 token、把 ; 当分隔符）无从插手。
            full = L'"' + wt + L"\" -p ";
            AppendWinQuotedArg(full, profileName.c_str());
        }
    }
    if (which == TERM_PWSH)
    {
        full = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File ";
        AppendWinQuotedArg(full, shimPath.c_str());
        flags = CREATE_NEW_CONSOLE;
    }
    else if (which == TERM_VSCODE)
    {
        // VS Code 没有"用命令行让集成终端跑一条命令"的接口，正规做法是 Remote-SSH：
        // 复用优先：站点绑定了**用户自己的** SSH Host 就直接用它（主机/端口/密钥/agent 全照旧）；
        // 绑定的是我们自己生成的受管别名（或还没绑定）时，按当前站点信息把它重写一遍 ——
        // 这样一条指向已删除私钥的 IdentityFile 不会永远卡在那里。
        std::wstring alias;
        if (site.sshAlias[0] && _wcsicmp(site.sshAlias, ManagedAliasForSite(site).c_str()) != 0)
        {
            alias = site.sshAlias;
            ProbeLog(L"[TERM] vscode using user's own ssh alias '%s'", alias.c_str());
        }
        else if (FindUserSshAliasForSite(site, alias))
        {
            // 用户自己的 Host 优先于我们生成的受管块 —— 否则 VS Code 连上的是
            // "服务程序造出来的那个连接"，而不是用户已有的 SSH 配置。
            ProbeLog(L"[TERM] vscode reusing user's ssh host '%s'", alias.c_str());
        }
        else if (!EnsureSshConfigAlias(site, alias))
        {
            MessageBoxW(hwnd,
                ExplorerText(L"error.terminal_ssh_config",
                             L"无法写入 %USERPROFILE%\\.ssh\\config。",
                             L"Could not write %USERPROFILE%\\.ssh\\config."),
                ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK | MB_ICONERROR);
            return;
        }

        std::wstring codeCmd;
        if (!ResolveCodeCmd(codeCmd))
        {
            MessageBoxW(hwnd,
                ExplorerText(L"error.terminal_no_vscode",
                             L"未找到 VS Code 的 code.cmd。请确认已安装 VS Code（或用 code 命令所在的安装目录）。",
                             L"VS Code's code.cmd was not found. Make sure VS Code is installed."),
                ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK | MB_ICONERROR);
            return;
        }
        ProbeLog(L"[TERM] resolved code.cmd='%s'", codeCmd.c_str());

        // 先落请求文件：已经连着该主机的 VS Code 窗口会在 2 秒内接单，直接在那个窗口里
        // 新开终端（不新开窗口、不再弹工作区信任）。
        WriteVscodeRequest(alias.c_str(), remoteDir);

        // 关键：已有窗口连着这个主机时**绝不启动窗口**。否则不同目录会各开一个新窗口，
        // 每个新窗口都要再问一次「是否信任此工作区」——这正是之前的错误实现。
        if (ErfHasLiveVscodeWindow(alias.c_str()))
        {
            ProbeLog(L"[TERM] vscode: live window already connected to '%s' -> request file only, no launch", alias.c_str());
            // 终端会在那个窗口里出现；顺手把窗口请到前台，用户不必自己去任务栏找。
            FocusVscodeWindowAsync(alias.c_str(), 4000);
            return;                                   // 不启动任何东西，任务已交给扩展
        }

        // .cmd 必须经 cmd.exe；最外层再包一对引号，是 cmd /c 处理"含空格路径"的常规写法。
        full = L"cmd.exe /c \"\"" + codeCmd + L"\" --remote ssh-remote+" + alias + L" ";
        AppendWinQuotedArg(full, (remoteDir && remoteDir[0]) ? remoteDir : L"/");
        full += L"\"";
        flags = CREATE_NEW_CONSOLE;
    }

    ProbeLog(L"[TERM] launch site='%s' type='%s' which=%d dir='%s' shim='%s' cmd=%s",
             site.name, site.type, which, (remoteDir && remoteDir[0]) ? remoteDir : L"/",
             shimPath.c_str(), full.c_str());
    ProbeLog(L"[TERM] ssh: %s", sshCmdText.c_str());

    std::vector<WCHAR> buf(full.begin(), full.end());
    buf.push_back(0);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(NULL, buf.data(), NULL, NULL, FALSE, flags, NULL, NULL, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    else
    {
        ProbeLog(L"[TERM] CreateProcess failed err=%lu", GetLastError());
        MessageBoxW(hwnd,
            ExplorerText(L"error.terminal_failed",
                         L"无法启动终端（未找到 wt.exe 或 powershell.exe）。",
                         L"Could not start the terminal (wt.exe or powershell.exe not found)."),
            ExplorerText(L"dialog.remote", L"远程操作", L"Remote"), MB_OK | MB_ICONERROR);
    }
}

// 终端偏好：**站点级优先**（connections.json 的 Terminal，由服务程序的「终端配置」
// 标签页设置），站点没设才用全局（HKCU\Software\ExplorerRemoteFs\Terminal）。
// 取值：wt / powershell / vscode（大小写不敏感，允许 "windows-terminal"、"pwsh"、"code" 等别名）。
// 默认 wt；取不到或无法识别时回落到 wt（再由 LaunchTerminalForSite 按可用性降级）。
static int ParseTerminalName(PCWSTR val)
{
    if (!val || !val[0]) return -1;                     // 未设置
    if (0 == StrCmpIW(val, L"powershell") || 0 == StrCmpIW(val, L"pwsh")) return TERM_PWSH;
    if (0 == StrCmpIW(val, L"vscode") || 0 == StrCmpIW(val, L"code")) return TERM_VSCODE;
    if (0 == StrCmpIW(val, L"wt") || 0 == StrCmpIW(val, L"windows-terminal")) return TERM_WT;
    return -1;                                          // 无法识别 → 当作未设置
}

static int ReadGlobalTerminalPreference()
{
    WCHAR val[64] = {};
    DWORD cb = sizeof(val) - sizeof(WCHAR);
    DWORD type = 0;
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\ExplorerRemoteFs", 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(key, L"Terminal", NULL, &type, (LPBYTE)val, &cb) != ERROR_SUCCESS || type != REG_SZ)
            val[0] = 0;
        RegCloseKey(key);
    }
    int parsed = ParseTerminalName(val);
    return parsed >= 0 ? parsed : TERM_WT;
}

// 站点级设置覆盖全局；两者都取不到时用 wt。
static int ReadTerminalForSite(const FTPSITE &site)
{
    int sitePref = ParseTerminalName(site.terminal);
    if (sitePref >= 0)
    {
        ProbeLog(L"[TERM] terminal preference: site='%s' -> %d", site.name, sitePref);
        return sitePref;
    }
    int globalPref = ReadGlobalTerminalPreference();
    ProbeLog(L"[TERM] terminal preference: site='%s' (unset) -> global %d", site.name, globalPref);
    return globalPref;
}

class CMenu : public IContextMenu, public IShellExtInit, public IObjectWithSite {
public:
 CMenu():ref(1),data(NULL),site(NULL),m_pidlFolder(NULL){DllAddRef();}
 HRESULT QueryInterface(REFIID r,void**p){static const QITAB q[]={QITABENT(CMenu,IContextMenu),QITABENT(CMenu,IShellExtInit),QITABENT(CMenu,IObjectWithSite),{0}};return QISearch(this,q,r,p);}
 ULONG AddRef(){return InterlockedIncrement(&ref);} ULONG Release(){long n=InterlockedDecrement(&ref);if(!n)delete this;return n;}
 HRESULT QueryContextMenu(HMENU m,UINT i,UINT first,UINT,UINT flags){
    BOOL defaultOnly = (flags&CMF_DEFAULTONLY) != 0;
    SELDATA sel; if(!CollectSelection(data,&sel)) return MAKE_HRESULT(SEVERITY_SUCCESS,0,0);
    // Double-click probe (P0-1): log what Explorer asks for so the default-verb
    // path can be traced if double-click still does nothing.
    ProbeLog(L"[DBLCLK] CMenu::QueryContextMenu flags=0x%08X defaultOnly=%d site='%s' n=%d first='%s'",
             flags,(int)defaultOnly,sel.site,sel.count,sel.count?sel.names[0]:L"");
    // Site-picker items (no site segment in the folder PIDL) get the system
    // default menu (Open/Pin/Rename/Delete/Properties) only — our WinSCP-style
    // commands operate on remote files, not on saved connections.
    if(!sel.site[0]){
        // 站点行（站点选择器里的一行）：只加一个「在此打开终端」，落到该站点配置的 StartPath。
        // 其余仍交给系统默认菜单——我们的文件级动词对"保存的连接"本身没有意义。
        UINT added = 0;
        FTPSITE rowSite = {};
        if(sel.count == 1 && FindSiteByName(sel.names[0], &rowSite) && SiteIsSshCapable(rowSite.type)){
            InsertMenuW(m, i++, MF_BYPOSITION, first+MENU_TERMINAL,
                        ExplorerText(L"menu.terminal", L"在此打开终端", L"Open terminal here"));
            added = 1;
        }
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, added);
    }
    BOOL multi = sel.count>1;
    // Explorer itself owns opening folders.  Advertising our file Open verb for
    // a folder can make the first double-click invoke it instead of navigation.
    if(!sel.firstIsFolder){
        InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_OPEN,ExplorerText(L"menu.open",L"打开",L"Open"));
        SetMenuDefaultItem(m,first+MENU_OPEN,FALSE);
        if(defaultOnly) return MAKE_HRESULT(SEVERITY_SUCCESS,0,1);
        InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_EDIT,ExplorerText(L"menu.edit",L"编辑",L"Edit"));
        InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_DOWNLOAD,ExplorerText(L"menu.download",L"下载",L"Download"));
        InsertMenuW(m,i++,MF_BYPOSITION|MF_SEPARATOR,0,NULL);
    }
    if(defaultOnly) return MAKE_HRESULT(SEVERITY_SUCCESS,0,0);
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_COPY_CLIP,ExplorerText(L"menu.copy_clipboard",L"复制到剪贴板",L"Copy to clipboard"));
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_COPY_NAME,ExplorerText(L"menu.copy_name",L"复制文件名",L"Copy file name"));
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_COPY_NATIVE,ExplorerText(L"menu.copy_remote_path",L"复制远程路径",L"Copy remote path"));
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_COPY_FULL,ExplorerText(L"menu.copy_full_path",L"复制完整路径",L"Copy full path"));
    InsertMenuW(m,i++,MF_BYPOSITION|MF_SEPARATOR,0,NULL);
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_RCOPY,multi?ExplorerText(L"menu.duplicate_all",L"复制到...（全部）",L"Copy to... (all)"):ExplorerText(L"menu.duplicate",L"复制到...",L"Copy to..."));
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_RMOVE,multi?ExplorerText(L"menu.move_all",L"移动到...（全部）",L"Move to... (all)"):ExplorerText(L"menu.move",L"移动到...",L"Move to..."));
    InsertMenuW(m,i++,MF_BYPOSITION|(multi?MF_GRAYED:0),first+MENU_RENAME,ExplorerText(L"menu.rename",L"重命名",L"Rename"));
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_DELETE,ExplorerText(L"menu.delete",L"从服务器删除",L"Delete from server"));
    // 在此打开终端：只对 SSH 类站点出现（FTP 没有 shell 通道），且单选时目录才明确。
    {
        FTPSITE termSite = {};
        if(!multi && FindSiteByName(sel.site, &termSite) && SiteIsSshCapable(termSite.type))
            InsertMenuW(m, i++, MF_BYPOSITION, first+MENU_TERMINAL,
                        ExplorerText(L"menu.terminal", L"在此打开终端", L"Open terminal here"));
    }
    int custom=0; CUSTCMD cmds[MAX_CUSTOM]={};
    if(!multi){ custom=LoadCustomCommands(cmds,MAX_CUSTOM); if(custom>0) InsertMenuW(m,i++,MF_BYPOSITION|MF_SEPARATOR,0,NULL);
        for(int k=0;k<custom;k++) InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_CUSTOM_BASE+k,cmds[k].name); }
    InsertMenuW(m,i++,MF_BYPOSITION|MF_SEPARATOR,0,NULL);
    InsertMenuW(m,i++,MF_BYPOSITION,first+MENU_PROPERTIES,multi?ExplorerText(L"menu.properties_first",L"属性（第一个）",L"Properties (first)"):ExplorerText(L"menu.properties",L"属性",L"Properties"));
    return MAKE_HRESULT(SEVERITY_SUCCESS,0,13+custom);
 }
 HRESULT InvokeCommand(LPCMINVOKECOMMANDINFO ci){
    UINT id=IS_INTRESOURCE(ci->lpVerb)?LOWORD((UINT_PTR)ci->lpVerb):99;
    if(!data)return E_INVALIDARG;
    // HOST commands (Explorer's command bar buttons, keyboard accelerators,
    // shell automation) arrive as STRING canonical verbs, not our menu ids.
    // Bridge them onto the same handlers so the top bar works on our items
    // (2026-09-12). Without this, a string verb fell through to id=99 and the
    // call ended in E_INVALIDARG, i.e. the toolbar buttons did nothing.
    if(id==99){
        WCHAR verb[64]={};
        if(ci->cbSize>=sizeof(CMINVOKECOMMANDINFOEX)){
            LPCMINVOKECOMMANDINFOEX ex=(LPCMINVOKECOMMANDINFOEX)ci;
            if((ex->fMask&CMIC_MASK_UNICODE)&&ex->lpVerbW)
                StringCchCopyW(verb,ARRAYSIZE(verb),ex->lpVerbW);
        }
        if(!verb[0]&&ci->lpVerb)
            MultiByteToWideChar(CP_ACP,0,ci->lpVerb,-1,verb,ARRAYSIZE(verb));
        ProbeLog(L"[CMD] canonical verb '%s'",verb);
        if(0==StrCmpIW(verb,L"delete"))          id=MENU_DELETE;
        else if(0==StrCmpIW(verb,L"properties")) id=MENU_PROPERTIES;
        else if(0==StrCmpIW(verb,L"open"))       id=MENU_OPEN;
        else if(0==StrCmpIW(verb,L"edit"))       id=MENU_EDIT;
        else if(0==StrCmpIW(verb,L"download"))   id=MENU_DOWNLOAD;
        // Explorer's native "Copy path" command reaches the selected item
        // through this canonical verb.  Its public, pasteable ERF equivalent
        // is exactly the existing "Copy full path" operation.
        else if(0==StrCmpIW(verb,L"copyaspath")) id=MENU_COPY_FULL;
        else { ProbeLog(L"[CMD] verb '%s' not mapped",verb); return S_OK; }
    }
    SELDATA sel; if(!CollectSelection(data,&sel))return E_FAIL;
    ProbeLog(L"[DBLCLK] CMenu::InvokeCommand id=%u site='%s' n=%d",id,sel.site,sel.count);
    PCWSTR pnames[MAX_SEL]; for(int k=0;k<sel.count;k++) pnames[k]=sel.names[k];
    if(id>=MENU_CUSTOM_BASE){ RunCustomCommand(ci->hwnd,sel.site,sel.folder,sel.names[0],id-MENU_CUSTOM_BASE); if(sel.notify)CoTaskMemFree(sel.notify); return S_OK; }
    switch(id){
    case MENU_OPEN: for(int k=0;k<sel.count;k++) OpenRemote(ci->hwnd,sel.site,sel.folder,sel.names[k],FALSE); break;
    case MENU_EDIT: for(int k=0;k<sel.count;k++) OpenRemote(ci->hwnd,sel.site,sel.folder,sel.names[k],TRUE); break;
    case MENU_DOWNLOAD: DownloadFiles(ci->hwnd,sel.site,sel.folder,pnames,sel.count); break;
    case MENU_COPY_CLIP: CopyClipboard(ci->hwnd,sel.site,sel.folder,pnames,sel.count); break;
    case MENU_COPY_NAME:
        { std::wstring t; for(int k=0;k<sel.count;k++){ if(k)t+=L"\r\n"; t+=sel.names[k]; } CopyTextToClipboard(ci->hwnd,t.c_str()); break; }
    case MENU_COPY_NATIVE:
        { std::wstring t; for(int k=0;k<sel.count;k++){ if(k)t+=L"\r\n"; WCHAR full[700]; JoinPath(sel.folder,sel.names[k],full,ARRAYSIZE(full)); t+=full; } CopyTextToClipboard(ci->hwnd,t.c_str()); break; }
    case MENU_COPY_FULL:
        { std::wstring t; for(int k=0;k<sel.count;k++){ if(k)t+=L"\r\n"; WCHAR full[700]; JoinPath(sel.folder,sel.names[k],full,ARRAYSIZE(full)); t+=L"erf:"; t+=sel.site; t+=L":"; t+=full; } CopyTextToClipboard(ci->hwnd,t.c_str()); break; }
    case MENU_RCOPY: ServerCopy(ci->hwnd,sel.site,sel.folder,pnames,sel.count,sel.notify); break;
    case MENU_RMOVE: ServerMove(ci->hwnd,sel.site,sel.folder,pnames,sel.count,sel.notify); break;
    case MENU_RENAME: DoRename(ci->hwnd,sel.site,sel.folder,sel.names[0],sel.notify); break;
    case MENU_DELETE:
    {
        // Do not duplicate a custom confirmation/CLI loop here.  The native
        // IFileOperation obtains our ITransferSource and funnels the actual
        // mutation through DeleteRemoteShellItem, exactly like the top button.
        HRESULT hrDelete = DeleteSelectionWithNativeFileOperation(ci->hwnd, data);
        if (FAILED(hrDelete) && !IsNativeDeleteCancelled(hrDelete))
            MessageBoxW(ci->hwnd, ExplorerText(L"error.some_deletes_failed",L"删除失败。",L"Delete failed."),
                        ExplorerText(L"dialog.remote",L"远程操作",L"Remote"), MB_OK|MB_ICONERROR);
        break;
    }
    case MENU_PROPERTIES:{
        WCHAR path[600] = {};
        JoinPath(sel.folder,sel.names[0],path,ARRAYSIZE(path));
        // 不同步 ReadRemoteMeta，也不用 DialogBoxParamW：两者都会把 Explorer 的
        // UI 路径拖进网络等待或 owner-modal 禁用状态。modeless 页先展示缓存，
        // 缓存未命中时由 MetaWarmThread 异步补齐。
        ShowRemotePropertiesSheetModeless(sel.site, sel.folder, sel.names[0], path,
                                     SiteCanSetOwner(sel.site));
        break; }
    case MENU_TERMINAL:
    {
        // 目标目录：站点内 → 当前目录（选中的是文件夹就再进一层）；站点行 → 该站点的 StartPath。
        FTPSITE ts = {};
        WCHAR dir[512] = {};
        if(sel.site[0]){
            if(FindSiteByName(sel.site, &ts)){
                StringCchCopyW(dir, ARRAYSIZE(dir), sel.folder);
                if(sel.count == 1 && sel.firstIsFolder && sel.names[0][0]){
                    size_t len = wcslen(dir);
                    if(len && dir[len - 1] != L'/') StringCchCatW(dir, ARRAYSIZE(dir), L"/");
                    StringCchCatW(dir, ARRAYSIZE(dir), sel.names[0]);
                }
            }
        }
        else if(sel.count == 1 && FindSiteByName(sel.names[0], &ts)){
            StringCchCopyW(dir, ARRAYSIZE(dir), ts.startPath);
        }
        if(ts.name[0])
        {
            // 终端：站点级优先（服务程序「终端配置」标签页），其次全局设置。
            int which = ReadTerminalForSite(ts);
            LaunchTerminalForSite(ci->hwnd, ts, dir, which);
        }
        break;
    }
    default: if(sel.notify)CoTaskMemFree(sel.notify); return E_INVALIDARG;
    }
    if(sel.notify) CoTaskMemFree(sel.notify);
    return S_OK;
 }
 HRESULT GetCommandString(UINT_PTR id,UINT type,UINT*,LPSTR s,UINT c){
    PCWSTR v=L"";
    switch(id){case MENU_OPEN:v=L"open";break;case MENU_EDIT:v=L"edit";break;case MENU_DOWNLOAD:v=L"download";break;
      case MENU_COPY_CLIP:v=L"copy_to_clipboard";break;case MENU_COPY_NAME:v=L"copy_file_name";break;
      case MENU_COPY_NATIVE:v=L"copy_remote_path";break;case MENU_COPY_FULL:v=L"copyaspath";break;
      case MENU_RCOPY:v=L"remote_copy";break;case MENU_RMOVE:v=L"remote_move";break;case MENU_RENAME:v=L"rename";break;
      case MENU_DELETE:v=L"delete";break;case MENU_PROPERTIES:v=L"properties";break;
      case MENU_TERMINAL:v=L"openterminal";break;
      default:return E_NOTIMPL;}
    // Probe (2026-09-12): the shell asks which canonical verbs we support
    // before wiring up command-bar buttons / context items. Logging the query
    // shows which host commands are actually offered to a namespace extension.
    ProbeLog(L"[CMD] GetCommandString id=%u type=%u -> '%s'",(UINT)id,type,v);
    if(type==GCS_VERBW) return StringCchCopyW((PWSTR)s,c,v);
    if(type==GCS_VERBA){ char a[64]; WideCharToMultiByte(CP_ACP,0,v,-1,a,ARRAYSIZE(a),NULL,NULL); return StringCchCopyA(s,c,a); }
    return E_NOTIMPL;
 }
 HRESULT Initialize(PCIDLIST_ABSOLUTE pidlFolder,IDataObject*d,HKEY){
    ProbeLog(L"[MENU] CMenu::Initialize pidlFolder=%p data=%p", pidlFolder, d);
    if(data)data->Release();data=d;if(data)data->AddRef();
    if(m_pidlFolder)ILFree(m_pidlFolder);
    m_pidlFolder=pidlFolder?ILCloneFull(pidlFolder):NULL;
    return S_OK;}
 HRESULT SetSite(IUnknown*s){if(site)site->Release();site=s;if(site)site->AddRef();return S_OK;} HRESULT GetSite(REFIID r,void**p){return site?site->QueryInterface(r,p):E_FAIL;}
private:
 ~CMenu(){if(data)data->Release();if(site)site->Release();if(m_pidlFolder)ILFree(m_pidlFolder);DllRelease();}
 long ref;IDataObject*data;IUnknown*site;PIDLIST_ABSOLUTE m_pidlFolder;
};
HRESULT CFolderViewImplContextMenu_CreateInstance(REFIID riid,void**ppv){*ppv=NULL;CMenu*m=new(std::nothrow)CMenu();if(!m)return E_OUTOFMEMORY;HRESULT hr=m->QueryInterface(riid,ppv);m->Release();return hr;}

// ---- property sheet: Ribbon "Properties" button -> standard Properties
// dialog with our Permissions page (PSN_APPLY writes back via chmod). --------

// Read-only connection info page for a saved site (site-picker level 0).
static INT_PTR CALLBACK SitePageProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        prop_probe::ProbeScope _probe(L"SitePage WM_INITDIALOG", 50);
        prop_probe::DumpChain(hDlg, L"SitePage init");
        prop_probe::Start(hDlg);
        PROPMETA *pm = (PROPMETA*)((LPPROPSHEETPAGE)lp)->lParam;
        if (!pm) return FALSE;
        SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)pm);
        SetWindowTextW(hDlg, ExplorerText(L"property.site_properties", L"站点属性", L"Site properties"));
        LocalizeSiteDialog(hDlg);
        PermHideApplyButton(hDlg);   // 站点页是只读的，「应用」同样没有意义
        PopulateSiteInfo(hDlg, pm->site);
        return TRUE;
    }
    case WM_NOTIFY:
    {
        NMHDR *nm = (NMHDR*)lp;
        if (nm && nm->code == PSN_APPLY)
        {
            SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}
static UINT CALLBACK PermPageCallback(HWND /* hwnd */, UINT uMsg, LPPROPSHEETPAGE ppsp)
{
    if (uMsg == PSPCB_CREATE)
    {
        // 属性表**即将**创建页面。此刻还没 hDlg，只能记一笔时间线；
        // 窗口链 dump 放在 WM_INITDIALOG（那时宿主结构已经成型）。
        ProbeLog(L"[PROBE] PSPCB_CREATE (tid=%lu)", GetCurrentThreadId());
    }
    if (uMsg == PSPCB_RELEASE)
    {
        prop_probe::Stop();   // 属性页关掉了，采样线程收工
        ProbeLog(L"[PROBE] PSPCB_RELEASE (tid=%lu)", GetCurrentThreadId());

        // 提示气泡是我们自己创建的顶层窗口，页面释放时必须销毁 ——
        // 以前从不销毁：每打开一次属性页就漏一个 TOOLTIPS_CLASS 窗口。
        if (g_permTip) { DestroyWindow(g_permTip); g_permTip = NULL; }

        // 页面放手：后台取元数据的线程可能还持有它（refs），谁最后放手谁释放。
        PropMetaRelease((PROPMETA*)ppsp->lParam);
    }
    return 1;
}

static INT_PTR CALLBACK PermPageProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        prop_probe::ProbeScope _probe(L"PermPage WM_INITDIALOG", 50);
        ProbeLog(L"[DIAG] PermPage WM_INITDIALOG created");
        // Win11 取证：页面 → 根的完整窗口链（谁 disabled、owner 是谁、宿主长什么样）
        // + 起采样线程盯着"其他窗口什么时候被禁用/卡住"。
        prop_probe::DumpChain(hDlg, L"PermPage init");
        prop_probe::Start(hDlg);
        PROPMETA *pm = (PROPMETA*)((LPPROPSHEETPAGE)lp)->lParam;
        if (!pm) return FALSE;
        SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)pm);
        pm->dlg = hDlg;                 // 后台元数据回来时 PostMessage 到这里
        LocalizePermissionDialog(hDlg);
        PermHideApplyButton(hDlg);
        PermMakeValueFieldsFlat(hDlg);
        if (pm->canSetOwner) PermAttachInputTooltip(hDlg);
        PermClearValueSelection(hDlg);
        PermFillMeta(hDlg, pm);
        PermApplyAsyncMeta(hDlg);       // 冷缓存时后台可能已经先回来了
        return TRUE;
    }
    case WM_APP_ERF_META_READY:
        PermApplyAsyncMeta(hDlg);
        return TRUE;
    case WM_COMMAND:
        if(HIWORD(wp)==BN_CLICKED && LOWORD(wp)>=3011 && LOWORD(wp)<=3019){ PermSyncChecksToOctal(hDlg); return TRUE; }
        if(HIWORD(wp)==EN_CHANGE && LOWORD(wp)==3022){ PermSyncOctalToChecks(hDlg); return TRUE; }
        break;
    case WM_CTLCOLORSTATIC:
    {
        // 只读值框（3001~3007）的观感：见 PermColorReadOnlyValue。
        INT_PTR br = PermColorReadOnlyValue(hDlg, (HWND)lp, wp);
        if (br) return br;
        break;
    }
    case WM_NOTIFY:
    {
        NMHDR *nm = (NMHDR*)lp;
        if (nm && nm->code == PSN_APPLY)
        {
            PROPMETA *pm = (PROPMETA*)GetWindowLongPtrW(hDlg, DWLP_USER);
            if (pm)
            {
                PermSyncOctalToChecks(hDlg);
                DWORD mode = PermCollectChecks(hDlg);
                BOOL recursive = IsDlgButtonChecked(hDlg,3023)!=0;
                if (mode != pm->meta.bits || recursive)
                {
                    WCHAR modeStr[8]; StringCchPrintf(modeStr,ARRAYSIZE(modeStr),L"%03o",mode);
                    if (recursive)
                    {
                        // 递归改权限：属性页的「应用」在 Explorer 的 UI 线程上，
                        // 同步跑 CLI 会把整个属性页卡死（树大时还会撞上 RunCli 的 30 秒超时被杀）。
                        // 交给常驻服务：遍历在它自己的线程上，带进度窗口与「取消」。
                        StartChmodRecursiveAsync(pm->site, pm->path, modeStr);
                    }
                    else
                    {
                        // 非递归 chmod 同样走后台：RunCli 起新进程 + 新连接，
                        // 在「应用」的 UI 线程上等就是一次假死（2026-09-20）。
                        BgCliJob job;
                        BgAddStep(job, pm->site, L"chmod", pm->path, modeStr);
                        StringCchCopy(job.refreshSite, ARRAYSIZE(job.refreshSite), pm->site);
                        PathParent(pm->path, job.refreshFolder, ARRAYSIZE(job.refreshFolder));
                        job.notify = pm->notify ? ILCloneFull(pm->notify) : NULL;
                        StringCchCopy(job.failText, ARRAYSIZE(job.failText),
                            ExplorerText(L"error.chmod_failed", L"权限修改失败。", L"Permission update failed."));
                        StartBgCliJob(job);
                    }
                }
                PermApplyChown(hDlg, pm);
            }
            SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

// ---- modeless 属性表的按钮必须自己接管 -------------------------------------
// 属性表的「确定 / 取消 / 关闭」按钮属于属性表 frame，不属于我们提供的页面，
// 所以页面过程（PermPageProc）根本收不到这些 WM_COMMAND —— 模态属性表由它自己的
// 内部模态循环收尾，而 PSH_MODELESS 下那个循环并不存在：frame 收下点击后什么都不做，
// 窗口就一直留在屏幕上。2026-09-19 Win11 实测症状：三个按钮全部无反应，只能关掉
// Explorer 主窗口，属性页才跟着消失。
// 处理：子类化 sheet frame —— 先交给 frame 自己处理（「确定」必须由它把 PSN_APPLY
// 通知发给页面，chmod 才会写回），再显式 DestroyWindow。
static const UINT_PTR kSheetSubclassId = 1;

static LRESULT CALLBACK ErfSheetFrameSubclassProc(HWND hWnd, UINT uMsg, WPARAM wp, LPARAM lp,
                                                  UINT_PTR /*uIdSubclass*/, DWORD_PTR /*dwRefData*/)
{
    switch (uMsg)
    {
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDCLOSE)
        {
            LRESULT r = DefSubclassProc(hWnd, uMsg, wp, lp);   // frame 先发 PSN_APPLY（点确定时）
            ProbeLog(L"[DIAG] sheet frame button cmd=%u -> close modeless sheet hwnd=%p",
                     (unsigned)LOWORD(wp), (void*)hWnd);
            if (IsWindow(hWnd)) DestroyWindow(hWnd);
            return r;
        }
        break;
    case WM_CLOSE:
        ProbeLog(L"[DIAG] sheet frame WM_CLOSE -> close modeless sheet hwnd=%p", (void*)hWnd);
        DestroyWindow(hWnd);
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE)   // 右上角 X / Alt+F4
        {
            ProbeLog(L"[DIAG] sheet frame SC_CLOSE -> close modeless sheet hwnd=%p", (void*)hWnd);
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    }
    return DefSubclassProc(hWnd, uMsg, wp, lp);
}

// 右键菜单/背景菜单的“属性”过去创建 IDD_PERMBOX：它是灰底、无标签的
// 自定义对话框；早期更以 Explorer HWND 调 DialogBoxParamW，因而会锁住
// 同一宿主的其他窗口。这里改为与 Explorer 原生属性一致的 PropertySheet：
// 使用 IDD_PERMPAGE、标准标签页和按钮，但 PSH_MODELESS + NULL parent，
// 所以它既有第二张截图的外观，又不会禁用任何 Explorer 窗口。
static void ShowRemotePropertiesSheetModeless(PCWSTR site, PCWSTR folder, PCWSTR name,
                                              PCWSTR fullPath, BOOL canSetOwner)
{
    if (!site || !site[0] || !folder || !name || !name[0] || !fullPath || !fullPath[0]) return;

    PROPMETA *pm = (PROPMETA*)CoTaskMemAlloc(sizeof(*pm));
    if (!pm) return;
    ZeroMemory(pm, sizeof(*pm));
    pm->refs = 1;
    pm->modeless = TRUE;
    pm->canSetOwner = canSetOwner;
    StringCchCopy(pm->site, ARRAYSIZE(pm->site), site);
    StringCchCopy(pm->folder, ARRAYSIZE(pm->folder), folder);
    StringCchCopy(pm->name, ARRAYSIZE(pm->name), name);
    StringCchCopy(pm->path, ARRAYSIZE(pm->path), fullPath);

    const BOOL haveMeta = ReadRemoteMetaCached(site, folder, name, &pm->meta);
    PROPSHEETPAGE psp = {};
    psp.dwSize = sizeof(psp);
    psp.dwFlags = PSP_USECALLBACK | PSP_USETITLE;
    psp.pszTitle = PropertyDialogTitle(&pm->meta);
    psp.hInstance = g_hInst;
    psp.pszTemplate = MAKEINTRESOURCEW(IDD_PERMPAGE);
    psp.pfnDlgProc = PermPageProc;
    psp.lParam = (LPARAM)pm;
    psp.pfnCallback = PermPageCallback;

    PROPSHEETHEADER psh = {};
    psh.dwSize = sizeof(psh);
    psh.dwFlags = PSH_MODELESS | PSH_PROPSHEETPAGE;
    psh.hwndParent = NULL;  // no Explorer owner: never enter the owner-modal disable chain
    psh.hInstance = g_hInst;
    psh.pszCaption = PropertyDialogTitle(&pm->meta);
    psh.nPages = 1;
    psh.ppsp = &psp;

    ProbeLog(L"[PROBE] properties standard-sheet modeless cache=%d owner=NULL site='%s' folder='%s' name='%s' path='%s'",
             (int)haveMeta, site, folder, name, fullPath);
    HWND sheet = (HWND)PropertySheetW(&psh);
    if (sheet && sheet != (HWND)-1)
    {
        // 挂上按钮处理：PSH_MODELESS 的属性表不会自己因确定/取消而关闭（见上面的说明）。
        if (!SetWindowSubclass(sheet, ErfSheetFrameSubclassProc, kSheetSubclassId, 0))
            ProbeLog(L"[DIAG] sheet frame subclass failed err=%lu（按钮可能仍无法关闭属性页）", GetLastError());
        ShowWindow(sheet, SW_SHOWNORMAL);
        SetForegroundWindow(sheet);
        if (!haveMeta) MetaWarmStart(pm);
    }
    else
    {
        // PropertySheet owns the PSP_USECALLBACK page once called and releases
        // its lParam through PermPageCallback, including failure paths.
        ProbeLog(L"[PROBE] properties standard-sheet create failed err=%lu", GetLastError());
    }
}
class CFolderViewImplPropSheet : public IShellPropSheetExt, public IShellExtInit
{
public:
    CFolderViewImplPropSheet() : ref(1), data(NULL) { DllAddRef(); }
    ~CFolderViewImplPropSheet() { if(data) data->Release(); DllRelease(); }

    HRESULT QueryInterface(REFIID r, void **p)
    {
        static const QITAB q[] = { QITABENT(CFolderViewImplPropSheet, IShellPropSheetExt),
                                   QITABENT(CFolderViewImplPropSheet, IShellExtInit), {0} };
        return QISearch(this, q, r, p);
    }
    ULONG AddRef() { return InterlockedIncrement(&ref); }
    ULONG Release() { long n = InterlockedDecrement(&ref); if(!n) delete this; return n; }

    // IShellExtInit: Explorer gives us the selection (IDataObject).
    HRESULT Initialize(PCIDLIST_ABSOLUTE, IDataObject *d, HKEY)
    {
        if (data) data->Release();
        data = d;
        if (data) data->AddRef();
        return S_OK;
    }

    // IShellPropSheetExt
    HRESULT AddPages(LPFNADDPROPSHEETPAGE pfnAddPage, LPARAM lParam)
    {
        // AddPages 跑在 Explorer 的 UI 线程上：这里的每一毫秒都是整个 shell 的停顿。
        // ProbeScope 超过 100ms 就留证据（Win11「其他窗口一起变暗」的直接原因）。
        prop_probe::ProbeScope _probe(L"AddPages total", 100);
        ProbeLog(L"[DIAG] PropSheet AddPages called, data=%p pfnAddPage=%p tid=%lu",
                 (void*)data, (void*)pfnAddPage, GetCurrentThreadId());
        if (!pfnAddPage || !data) return S_OK;   // no selection -> nothing to add
        SELDATA sel;
        if (!CollectSelection(data, &sel)) return S_OK;
        ProbeLog(L"[DIAG] PropSheet selection ok: site='%s' folder='%s' name='%s' count=%d",
                 sel.site, sel.folder, sel.names[0], sel.count);

        PROPMETA *pm = (PROPMETA*)CoTaskMemAlloc(sizeof(PROPMETA));
        if (!pm) return E_OUTOFMEMORY;
        ZeroMemory(pm, sizeof(*pm));
        pm->refs = 1;                    // AddPages 持有第一份引用
        StringCchCopy(pm->site, ARRAYSIZE(pm->site), sel.site);
        StringCchCopy(pm->folder, ARRAYSIZE(pm->folder), sel.folder);
        StringCchCopy(pm->name, ARRAYSIZE(pm->name), sel.names[0]);
        pm->canSetOwner = SiteCanSetOwner(sel.site);
        JoinPath(sel.folder, sel.names[0], pm->path, ARRAYSIZE(pm->path));
        pm->notify = sel.notify ? ILCloneFull(sel.notify) : NULL;
        if (sel.notify) CoTaskMemFree(sel.notify);
        // 属性页在 Explorer 的 UI 线程上：这里**绝不能**同步等远程。只读缓存；
        // 未命中就后台取（MetaWarm），页面用空值先开出来，取回来再刷新。
        BOOL haveMeta = ReadRemoteMetaCached(sel.site, sel.folder, sel.names[0], &pm->meta);
        const FTPSITE *siteItem = haveMeta ? NULL : FtpSiteFind(sel.names[0]);
        if (!haveMeta && sel.folder[0] && sel.site[0])
        {
            // 远程目录里的项但缓存未命中（冷目录）：后台线程去取真实元数据，
            // 页面先用空值开出来（毫秒级），取回来再 PostMessage 刷新。
            MetaWarmStart(pm);
            ProbeLog(L"[DIAG] PropSheet: meta cache miss, opening with empty values site='%s' path='%s' name='%s'",
                     sel.site, sel.folder, sel.names[0]);
        }
        else if (!haveMeta)
        {
            // Site-picker item (level 0): not a remote file — show a read-only
            // connection info page instead of a Permissions page.
            const FTPSITE *s = siteItem;
            if (!s)
            {
                PropMetaRelease(pm);
                return S_OK;
            }
            StringCchCopy(pm->site, ARRAYSIZE(pm->site), sel.names[0]);
            PROPSHEETPAGE psp = {};
            psp.dwSize = sizeof(psp);
            psp.dwFlags = PSP_USECALLBACK | PSP_USETITLE;
            psp.pszTitle = ExplorerText(L"property.site_properties", L"站点属性", L"Site properties");
            psp.hInstance = g_hInst;
            psp.pszTemplate = MAKEINTRESOURCEW(IDD_SITEPAGE);
            psp.pfnDlgProc = SitePageProc;
            psp.lParam = (LPARAM)pm;
            psp.pfnCallback = PermPageCallback;
            HPROPSHEETPAGE hPage = CreatePropertySheetPage(&psp);
            if (!hPage)
            {
                PropMetaRelease(pm);
                return S_OK;
            }
            if (!pfnAddPage(hPage, lParam))
                DestroyPropertySheetPage(hPage);
            return S_OK;
        }

        PROPSHEETPAGE psp = {};
        psp.dwSize = sizeof(psp);
        psp.dwFlags = PSP_USECALLBACK | PSP_USETITLE;
        psp.pszTitle = PropertyDialogTitle(&pm->meta);
        psp.hInstance = g_hInst;
        psp.pszTemplate = MAKEINTRESOURCEW(IDD_PERMPAGE);
        psp.pfnDlgProc = PermPageProc;
        psp.lParam = (LPARAM)pm;
        psp.pfnCallback = PermPageCallback;

        HPROPSHEETPAGE hPage = CreatePropertySheetPage(&psp);
        ProbeLog(L"[DIAG] PropSheet CreatePropertySheetPage hPage=%p lastErr=%u", (void*)hPage, GetLastError());
        if (!hPage)
        {
            PropMetaRelease(pm);
            return S_OK;
        }
        BOOL added = pfnAddPage(hPage, lParam);     // pass Explorer's lParam back verbatim
        ProbeLog(L"[DIAG] PropSheet pfnAddPage returned %d", added);
        if (!added)
        {
            DestroyPropertySheetPage(hPage);
        }
        return S_OK;
    }
    HRESULT ReplacePage(EXPPS, LPFNSVADDPROPSHEETPAGE, LPARAM) { return E_NOTIMPL; }

private:
    long ref;
    IDataObject *data;
};

HRESULT CFolderViewImplPropSheet_CreateInstance(REFIID riid, void **ppv)
{
    *ppv = NULL;
    CFolderViewImplPropSheet *m = new (std::nothrow) CFolderViewImplPropSheet();
    if (!m) return E_OUTOFMEMORY;
    HRESULT hr = m->QueryInterface(riid, ppv);
    m->Release();
    return hr;
}

// ---- background menu wrapper: system default menu + our folder commands -----
static CUSTCMD g_cmds[MAX_CUSTOM];
class CFolderViewImplBgMenu : public IContextMenu, public IObjectWithSite
{
public:
    CFolderViewImplBgMenu(IContextMenu *pDef, PCIDLIST_ABSOLUTE pidlFolder, int level)
        : ref(1), m_pDefault(pDef), m_site(NULL), m_lastFirst(0), m_defaultCount(0), m_hasTerminalItem(FALSE), m_nLevel(level)
    {
        if (m_pDefault) m_pDefault->AddRef();   // keep the default menu alive
        m_pidl = pidlFolder ? ILCloneFull(pidlFolder) : NULL;
        DllAddRef();
    }
    ~CFolderViewImplBgMenu()
    {
        if (m_pDefault) m_pDefault->Release();
        if (m_site) m_site->Release();
        if (m_pidl) ILFree(m_pidl);
        DllRelease();
    }
    HRESULT QueryInterface(REFIID r, void **p)
    {
        static const QITAB q[] = { QITABENT(CFolderViewImplBgMenu, IContextMenu),
                                   QITABENT(CFolderViewImplBgMenu, IObjectWithSite), {0} };
        return QISearch(this, q, r, p);
    }
    ULONG AddRef() { return InterlockedIncrement(&ref); }
    ULONG Release() { long n = InterlockedDecrement(&ref); if (!n) delete this; return n; }

    HRESULT QueryContextMenu(HMENU m, UINT i, UINT first, UINT maxid, UINT flags)
    {
        UINT n = 0;
        if (m_pDefault)
        {
            HRESULT hr = m_pDefault->QueryContextMenu(m, i, first, maxid, flags);
            if (SUCCEEDED(hr)) n = LOWORD(hr);
        }
        m_lastFirst = first;
        m_defaultCount = n;
        ProbeLog(L"[BG] QueryContextMenu first=%u maxid=%u flags=0x%X defaultCount=%u", first, maxid, flags, n);

        UINT pos = i + n;
        UINT our = first + n;
        UINT added = 0;
#define BG_INSERT(text) do { if (our < maxid) { InsertMenuW(m, pos++, MF_BYPOSITION, our++, (text)); added++; } } while(0)
        if (m_nLevel == 0)
        {
            // Site picker (connection manager): the only useful action here is
            // creating a new site, which launches the GUI site manager.
            BG_INSERT(ExplorerText(L"menu.new_site", L"新建站点...", L"New site..."));
        }
        else
        {
            BG_INSERT(ExplorerText(L"menu.copy_current_path", L"复制当前路径", L"Copy current path"));
            BG_INSERT(ExplorerText(L"menu.new_folder", L"新建文件夹...", L"New folder..."));
            BG_INSERT(ExplorerText(L"menu.new_file", L"新建文件...", L"New file..."));
            // level 1 is a site's remote root; every level from there has a
            // current-site configuration. Directory metadata is shown when available.
            BG_INSERT(ExplorerText(L"menu.current_directory_properties", L"显示当前目录属性", L"Current directory properties"));
            BG_INSERT(ExplorerText(L"menu.current_site_information", L"显示当前站点信息", L"Current site information"));
            // Entry point to the resident client's Transfers tab. The Explorer
            // command bar cannot host a persistent custom NSE button on Win10/11
            // (SFVM_GETBUTTONS targets the pre-Vista toolbar; IExplorerCommand-
            // Provider only yields selection-scoped commands), so the folder
            // context menu is the reliable launcher.
            BG_INSERT(ExplorerText(L"menu.transfer_queue", L"传输队列", L"Transfer queue"));

            // 「在此打开终端」：只对 SSH 站点（SFTP/SCP）出现。判定必须与
            // InvokeCommand 完全一致，否则菜单项与派发会错位。
            {
                WCHAR bgSite[64] = {};
                FTPSITE bgFt = {};
                if (m_pidl) PidlSite(m_pidl, bgSite, ARRAYSIZE(bgSite));
                if (bgSite[0] && FindSiteByName(bgSite, &bgFt) && SiteIsSshCapable(bgFt.type))
                {
                    if (our < maxid)
                    {
                        InsertMenuW(m, pos++, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
                        added++;
                    }
                    BG_INSERT(ExplorerText(L"menu.open_terminal_here", L"在此打开终端", L"Open terminal here"));
                    m_hasTerminalItem = TRUE;
                }
            }
        }
#undef BG_INSERT
        int custom = 0;
        if (added > 0 && our < maxid)
        {
            custom = LoadCustomCommands(g_cmds, MAX_CUSTOM);
            if (custom > 0)
            {
                InsertMenuW(m, pos++, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
                for (int k = 0; k < custom && our < maxid; k++)
                    InsertMenuW(m, pos++, MF_BYPOSITION, our++, g_cmds[k].name);
            }
        }
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, n + added + custom);
    }

    HRESULT InvokeCommand(LPCMINVOKECOMMANDINFO ci)
    {
        // Host-invoked CANONICAL VERB. Explorer's native paste command (Ctrl+V,
        // the toolbar Paste button, the native "Paste" context item) does NOT
        // call a namespace extension's IDropTarget — it asks the folder's
        // BACKGROUND context menu for a verb named "paste" (IContextMenu::
        // GetCommandString / GCS_VERBW) and then InvokeCommand(lpVerb="paste").
        // Do NOT register shell\paste in the registry: a lone verb under the
        // ProgID's shell key becomes the DEFAULT verb and hijacks double-click
        // (2026-09-12 regression: double-clicking a folder ran paste, so no
        // folder could be opened).
        LPCWSTR verbW = NULL;
        if (ci->cbSize >= sizeof(CMINVOKECOMMANDINFOEX))
        {
            LPCMINVOKECOMMANDINFOEX ex = (LPCMINVOKECOMMANDINFOEX)ci;
            if (ex->fMask & CMIC_MASK_UNICODE) verbW = ex->lpVerbW;
        }
        BOOL isPaste = verbW ? (0 == lstrcmpiW(verbW, L"paste"))
                             : (!IS_INTRESOURCE(ci->lpVerb) && ci->lpVerb && 0 == lstrcmpiA(ci->lpVerb, "paste"));
        BOOL isNew = verbW ? (0 == lstrcmpiW(verbW, L"new"))
                           : (!IS_INTRESOURCE(ci->lpVerb) && ci->lpVerb && 0 == lstrcmpiA(ci->lpVerb, "new"));
        BOOL isCopyAsPath = verbW ? (0 == lstrcmpiW(verbW, L"copyaspath"))
                                  : (!IS_INTRESOURCE(ci->lpVerb) && ci->lpVerb && 0 == lstrcmpiA(ci->lpVerb, "copyaspath"));
        if (isNew && m_nLevel >= 1)
        {
            WCHAR nsite[64] = {}, nfolder[512] = {};
            if (m_pidl) {
                PidlSite(m_pidl, nsite, ARRAYSIZE(nsite));
                PidlPath(m_pidl, nfolder, ARRAYSIZE(nfolder));
                ApplySiteStartPath(nsite, nfolder, ARRAYSIZE(nfolder));
            }
            ProbeLog(L"[BG] canonical verb new site='%s' folder='%s'", nsite, nfolder);
            NewFolderRemote(ci->hwnd, nsite, nfolder, m_pidl);
            return S_OK;
        }
        if (isCopyAsPath && m_nLevel >= 1)
        {
            WCHAR csite[64] = {}, cfolder[512] = {};
            if (m_pidl) {
                PidlSite(m_pidl, csite, ARRAYSIZE(csite));
                PidlPath(m_pidl, cfolder, ARRAYSIZE(cfolder));
                ApplySiteStartPath(csite, cfolder, ARRAYSIZE(cfolder));
            }
            std::wstring text = L"erf:"; text += csite; text += L":"; text += cfolder;
            ProbeLog(L"[BG] canonical verb copyaspath site='%s' folder='%s'", csite, cfolder);
            CopyTextToClipboard(ci->hwnd, text.c_str());
            return S_OK;
        }
        if (isPaste)
        {
            WCHAR vsite[64] = {}, vfolder[512] = {};
            if (m_pidl) {
                PidlSite(m_pidl, vsite, ARRAYSIZE(vsite));
                PidlPath(m_pidl, vfolder, ARRAYSIZE(vfolder));
                ApplySiteStartPath(vsite, vfolder, ARRAYSIZE(vfolder));
            }
            ProbeLog(L"[BG] canonical verb paste site='%s' folder='%s'", vsite, vfolder);
            if (m_nLevel >= 1) PasteClipboardToFolder(ci->hwnd, vsite, vfolder, m_pidl);
            return S_OK;
        }
        UINT id = IS_INTRESOURCE(ci->lpVerb) ? LOWORD((UINT_PTR)ci->lpVerb) : 99;
        // Explorer may pass either the absolute menu id (first+k) or the
        // relative index (k). Normalize to relative.
        UINT rel = id;
        if (m_lastFirst > 0 && id >= m_lastFirst) rel = id - m_lastFirst;
        ProbeLog(L"[BG] InvokeCommand id=%u rel=%u defaultCount=%u", id, rel, m_defaultCount);
        if (m_pDefault && rel < m_defaultCount)
            return m_pDefault->InvokeCommand(ci);           // system item

        WCHAR site[64] = {}, folder[512] = {};
        if (m_pidl) {
            PidlSite(m_pidl, site, ARRAYSIZE(site));
            PidlPath(m_pidl, folder, ARRAYSIZE(folder));
            ApplySiteStartPath(site, folder, ARRAYSIZE(folder));
        }
        UINT k = rel - m_defaultCount;
        if (m_nLevel == 0)
        {
            // Site picker: new site launches the GUI client's site manager.
            if (k == 0 && GetClientPath()[0])
                ShellExecuteW(ci->hwnd, NULL, GetClientPath(), L"--show", NULL, SW_SHOWNORMAL);
            return S_OK;
        }
        switch (k)
        {
        case 0: { std::wstring t = L"erf:"; t += site; t += L":"; t += folder; CopyTextToClipboard(ci->hwnd, t.c_str()); break; }
        case 1: NewFolderRemote(ci->hwnd, site, folder, m_pidl); break;
        case 2: NewFileRemote(ci->hwnd, site, folder, m_pidl); break;
        case 3: ShowCurrentFolderProperties(ci->hwnd, site, folder); break;
        case 4: ShowCurrentSiteInfo(ci->hwnd, site); break;
        case 5:
            // Open the resident client focused on the Transfers tab.
            if (GetClientPath()[0])
                ShellExecuteW(ci->hwnd, NULL, GetClientPath(), L"--transfers", NULL, SW_SHOWNORMAL);
            break;
        default:
            // 「在此打开终端」只在 SSH 站点插入（m_hasTerminalItem 与 QueryContextMenu 同判据），
            // 因此自定义命令的偏移要随之移动一位。
            if (m_hasTerminalItem && k == 6)
            {
                FTPSITE ts = {};
                if (FindSiteByName(site, &ts))
                    LaunchTerminalForSite(ci->hwnd, ts, folder, ReadTerminalForSite(ts));
                break;
            }
            BgCustomCommand(ci->hwnd, site, folder, (int)k - (m_hasTerminalItem ? 7 : 6));
            break;
        }
        return S_OK;
    }

    HRESULT GetCommandString(UINT_PTR id, UINT type, UINT *r, LPSTR s, UINT c)
    {
        // COM passes the menu id as an OFFSET from idCmdFirst. Normalize both
        // forms (offset or absolute) so we work regardless of caller.
        UINT off = (UINT)((id >= (UINT_PTR)m_lastFirst) ? (id - (UINT_PTR)m_lastFirst) : id);
        if (m_pDefault && off < m_defaultCount)
            return m_pDefault->GetCommandString(off, type, r, s, c);
        // Canonical verb names let HOST commands reach our items. Explorer's
        // native commands resolve the target folder's background menu through
        // canonical verbs. Item layout (level >= 1): k=0 copy path, k=1 new
        // folder.  k=2 is visually "New file", but continues to publish the
        // canonical paste verb: Explorer has no separate hidden-command API
        // for a virtual namespace background menu.  Native Ctrl+V invokes it
        // by verb, whereas a user click invokes the numeric menu id and opens
        // NewFileRemote above.
        if (m_nLevel >= 1 && m_pDefault && off == m_defaultCount + 0)
        {
            ProbeLog(L"[BG] GetCommandString copyaspath requested type=%u", type);
            if (type == GCS_VERBW) return StringCchCopyW((PWSTR)s, c, L"copyaspath");
            if (type == GCS_VERBA) return StringCchCopyA(s, c, "copyaspath");
        }
        if (m_nLevel >= 1 && m_pDefault && off == m_defaultCount + 2)
        {
            ProbeLog(L"[BG] GetCommandString paste-verb requested type=%u", type);
            if (type == GCS_VERBW) return StringCchCopyW((PWSTR)s, c, L"paste");
            if (type == GCS_VERBA) return StringCchCopyA(s, c, "paste");
        }
        // k=1 is "New folder" — expose it as the canonical "new" verb so the
        // shell's New command can reach us on the background.
        if (m_nLevel >= 1 && m_pDefault && off == m_defaultCount + 1)
        {
            ProbeLog(L"[BG] GetCommandString new-verb requested type=%u", type);
            if (type == GCS_VERBW) return StringCchCopyW((PWSTR)s, c, L"new");
            if (type == GCS_VERBA) return StringCchCopyA(s, c, "new");
        }
        return E_NOTIMPL;
    }

    HRESULT SetSite(IUnknown *s)
    {
        if (m_site) m_site->Release();
        m_site = s;
        if (m_site) m_site->AddRef();
        return S_OK;
    }
    HRESULT GetSite(REFIID r, void **p)
    {
        if (m_pDefault)
        {
            IObjectWithSite *ows = NULL;
            if (SUCCEEDED(m_pDefault->QueryInterface(IID_PPV_ARGS(&ows))))
            {
                HRESULT hr = ows->GetSite(r, p);
                ows->Release();
                return hr;
            }
        }
        return m_site ? m_site->QueryInterface(r, p) : E_FAIL;
    }

private:
    long ref;
    IContextMenu *m_pDefault;
    IUnknown *m_site;
    PIDLIST_ABSOLUTE m_pidl;
    UINT m_lastFirst;
    UINT m_defaultCount;
    BOOL m_hasTerminalItem = FALSE;   // level>=1 时是否插入了「在此打开终端」（派发偏移要用同一判据）
    int m_nLevel;
};

HRESULT CFolderViewImplBgMenu_Create(IContextMenu *pDef, PCIDLIST_ABSOLUTE pidlFolder, int level, REFIID riid, void **ppv)
{
    *ppv = NULL;
    CFolderViewImplBgMenu *bg = new (std::nothrow) CFolderViewImplBgMenu(pDef, pidlFolder, level);
    if (!bg) return E_OUTOFMEMORY;
    HRESULT hr = bg->QueryInterface(riid, ppv);
    bg->Release();
    return hr;
}
