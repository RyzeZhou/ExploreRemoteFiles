#pragma once
// ---------------------------------------------------------------------------
// RemoteDataObject.h — a virtual-file IDataObject for remote items.
//
// Two content paths:
//   * TOP-LEVEL FILE       -> CRemoteStream  (one CLI 'get' per file, one job)
//   * FILE INSIDE A FOLDER -> CFolderFetch   (ONE CLI 'getr' for the whole
//                                            tree, one job, WinSCP-style)
// The folder path mirrors WinSCP's queue model: "each entry in the queue
// represents one background transfer (not a file)" — a directory copy is a
// single job that scans the tree, sums the total size, then transfers every
// file over the same session while reporting cumulative bytes + the file in
// flight. Descriptors are produced from the listing walk (no download);
// contents are served from the local temp tree the 'getr' writes into.
// ---------------------------------------------------------------------------

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <string>
#include <vector>
#include <algorithm>
#include "ProbeLog.h"
#include "FtpMeta.h"

// ---------------------------------------------------------------------------
// IDataObjectAsyncCapability（{3D8B0590-F691-11d2-8EA9-006097DF5BD4}）
//
// 用途：告诉 Shell「这个数据对象可以**异步**取」。Explorer 的复制引擎在遇到慢数据源
// （网络/慢设备）时，会在后台线程上调用我们的 IDataObject::GetData，并用它自己的进度
// 窗口，而不是压住窗口线程。这正是"Ctrl+C 复制时源端 Explorer 假死"的对症解。
//
// 这里自带接口声明与 GUID，避免依赖 shldisp.h 及其 IID 所在库（ShlDisp.h 只做
// EXTERN_C 声明，定义在别的 lib 里）。若将来某处已 include 了 ShlDisp.h，用它自己的
// 接口定义，不重复声明。
// ---------------------------------------------------------------------------
#ifndef __IDataObjectAsyncCapability_INTERFACE_DEFINED__
#define __IDataObjectAsyncCapability_INTERFACE_DEFINED__
struct IDataObjectAsyncCapability : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL fDoOpAsync) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL *pfIsOpAsync) = 0;
    virtual HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx *pbcReserved) = 0;
    virtual HRESULT STDMETHODCALLTYPE InOperation(BOOL *pfInAsyncOp) = 0;
    virtual HRESULT STDMETHODCALLTYPE EndOperation(HRESULT hResult, IBindCtx *pbcReserved, DWORD dwEffects) = 0;
};
#endif
static const GUID ERF_IID_IDataObjectAsyncCapability =
{ 0x3d8b0590, 0xf691, 0x11d2, { 0x8e, 0xa9, 0x00, 0x60, 0x97, 0xdf, 0x5b, 0xd4 } };

// IShellItemResources {ff5693be-2ce0-4d48-b5c5-40817d1acdb9}
// 自带 GUID，避免依赖 ShObjIdl_core.h 里 EXTERN_C 的 IID 及其所在库。
static const GUID ERF_IID_IShellItemResources =
{ 0xff5693be, 0x2ce0, 0x4d48, { 0xb5, 0xc5, 0x40, 0x81, 0x7d, 0x1a, 0xcd, 0xb9 } };

// IEnumResources {2dd81fe3-a83c-4da9-a330-47249d345ba1}
static const GUID ERF_IID_IEnumResources =
{ 0x2dd81fe3, 0xa83c, 0x4da9, { 0xa3, 0x30, 0x47, 0x24, 0x9d, 0x34, 0x5b, 0xa1 } };

// Local path of the transfer CLI (per-user install location).
inline std::wstring RfsCliPath()
{
    WCHAR dir[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, dir))) return L"";
    std::wstring p = dir;
    p += L"\\ExplorerRemoteFs\\cli\\ExplorerRemoteFs.Cli.exe";
    return p;
}

// 单文件下载：**交给常驻服务**（见 FtpBridgeFetch 的说明）。
// 这里不再 CreateProcess(cli get) + WaitForSingleObject(INFINITE)：
// Shell DLL 不启动 CLI，会话复用 / 传输队列 / 进度 / 取消都归常驻服务。
// batchId 让"同一次复制"的所有文件在队列里归到一组。
// The typed result keeps a user cancellation distinct from every retryable or
// permanent download failure all the way to the IStream boundary.
inline FtpBridgeFetchState RfsFetchToFile(PCWSTR site, PCWSTR remote, PCWSTR local, PCWSTR batchId, std::string &reply)
{
    reply.clear();
    FtpBridgeFetchState state = FtpBridgeFetch(site, remote, local, batchId, reply);
    ProbeLog(L"[DATAOBJ] fetch(service) '%s' -> '%s' batch='%s' state=%d",
             remote, local, batchId ? batchId : L"-", (int)state);
    return state;
}
// ---------------------------------------------------------------------------
// Lazy stream: downloads once, then serves the local temp file.
// (Top-level single-file path.)
// ---------------------------------------------------------------------------
class CRemoteStream : public IStream
{
public:
    CRemoteStream(PCWSTR site, PCWSTR remote, ULONGLONG size, PCWSTR batchId)
        : _ref(1), _site(site ? site : L""), _remote(remote ? remote : L""),
          _batchId(batchId ? batchId : L""), _size(size), _pos(0),
          _h(INVALID_HANDLE_VALUE), _done(FALSE), _fetchStarted(FALSE),
          _downloadReady(FALSE), _cancelled(FALSE), _cancelRequested(FALSE)
    {
    }

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IStream) ||
            IsEqualIID(riid, IID_ISequentialStream))
        {
            *ppv = static_cast<IStream *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG n = InterlockedDecrement(&_ref);
        if (n == 0)
        {
            // CFSTR_FILECONTENTS is prefetched before the first byte reaches the
            // copy engine. _pos == 0 and _done == TRUE therefore do NOT mean the
            // download completed. A released stream after FETCH began is Explorer's
            // cancellation signal and must stop the resident service job.
            // 只有"**一个字节都没交出去**"的流在释放时才算取消信号：否则 Shell 只是
            // 用完释放（或 sniff 流提前释放），一发 CANCEL 会把**整批**（含正在跑的正主）
            // 一起取消 —— 实测表现为队列里两条任务、然后"磁盘操作失败"。
            if (_fetchStarted && !_jobDone && !_cancelled && !_cancelRequested &&
                !_batchId.empty() && _pos == 0)
            {
                _cancelRequested = TRUE;
                ProbeLog(L"[DL] cancel requested by stream release batch='%s' remote='%s' pos=%llu",
                         _batchId.c_str(), _remote.c_str(), (unsigned long long)_pos);
                std::string reply;
                BOOL confirmed = FtpBridgeCancel(_batchId.c_str(), reply);
                ProbeLog(L"[DL] cancel request confirmed=%d batch='%s' reply='%hs'",
                         (int)confirmed, _batchId.c_str(), reply.c_str());
            }
            delete this;
        }
        return n;
    }

    STDMETHODIMP Read(void *pv, ULONG cb, ULONG *pcbRead) override
    {
        if (pcbRead) *pcbRead = 0;
        if (!pv) return STG_E_INVALIDPOINTER;
        if (!Ensure())
        {
            if (_cancelled)
            {
                ProbeLog(L"[DL] Read cancelled site='%s' remote='%s' batch='%s'",
                         _site.c_str(), _remote.c_str(), _batchId.c_str());
                return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            }
            ProbeLog(L"[DL] Read FAILED at Ensure site='%s' remote='%s' local='%s'",
                     _site.c_str(), _remote.c_str(), _local.c_str());
            return STG_E_READFAULT;
        }

        // 边下边读：等到"这次要读的那一段"已经落盘（或任务终态）。
        const ULONGLONG tWait = GetTickCount64();
        for (;;)
        {
            if (_pos < OnDiskSize()) break;
            if (PollJobTerminal())
            {
                if (_pos < OnDiskSize()) break;
                if (_cancelled)
                {
                    ProbeLog(L"[DL] Read cancelled mid-stream site='%s' remote='%s' pos=%llu",
                             _site.c_str(), _remote.c_str(), (unsigned long long)_pos);
                    return HRESULT_FROM_WIN32(ERROR_CANCELLED);
                }
                if (_jobFailed && _pos == 0) return STG_E_READFAULT;
                break;   // 正常结束 → 下面 ReadFile 返回 EOF
            }
            Sleep(50);
        }

        ULONGLONG avail = OnDiskSize();
        if (_pos >= avail) { if (pcbRead) *pcbRead = 0; return S_FALSE; }   // EOF
        ULONGLONG canRead = avail - _pos;
        if ((ULONGLONG)cb > canRead) cb = (ULONG)canRead;

        DWORD got = 0;
        if (!ReadFile(_h, pv, cb, &got, NULL))
        {
            ProbeLog(L"[DL] ReadFile failed cb=%lu err=%lu local='%s'",
                     (unsigned long)cb, (unsigned long)GetLastError(), _local.c_str());
            return STG_E_READFAULT;
        }
        ULONGLONG waited = GetTickCount64() - tWait;
        ++_readCount;
        if (!_loggedFirstRead || waited >= 500 || _readCount <= 5)
        {
            _loggedFirstRead = TRUE;
            ProbeLog(L"[DL] Read tid=%lu n=%d cb=%lu got=%lu pos=%llu onDisk=%llu waitedMs=%llu local='%s'",
                     GetCurrentThreadId(), _readCount, (unsigned long)cb, (unsigned long)got,
                     (unsigned long long)_pos, (unsigned long long)avail, waited, _local.c_str());
        }
        if (pcbRead) *pcbRead = got;
        _pos += got;
        return got == 0 ? S_FALSE : S_OK;
    }
    STDMETHODIMP Write(const void *, ULONG, ULONG *) override { return STG_E_ACCESSDENIED; }

    STDMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *newPos) override
    {
        if (!Ensure()) return _cancelled ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : STG_E_READFAULT;
        LARGE_INTEGER zero = {};
        if (!SetFilePointerEx(_h, move, &zero, origin)) return STG_E_INVALIDFUNCTION;
        _pos = (ULONGLONG)zero.QuadPart;
        if (newPos) newPos->QuadPart = _pos;
        return S_OK;
    }
    STDMETHODIMP SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    STDMETHODIMP CopyTo(IStream *dst, ULARGE_INTEGER cb, ULARGE_INTEGER *read, ULARGE_INTEGER *written) override
    {
        if (!dst) return STG_E_INVALIDPOINTER;
        if (!Ensure()) return _cancelled ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : STG_E_READFAULT;
        std::vector<BYTE> buf(64 * 1024);
        ULONGLONG totalRead = 0, totalWritten = 0;
        while (totalRead < cb.QuadPart)
        {
            ULONGLONG want64 = (ULONGLONG)buf.size();
            if (want64 > cb.QuadPart - totalRead) want64 = cb.QuadPart - totalRead;
            ULONG want = (ULONG)want64;
            ULONG got = 0;
            HRESULT hrRead = Read(buf.data(), want, &got);
            if (FAILED(hrRead)) return hrRead;
            if (got == 0) break;
            ULONG put = 0;
            HRESULT hrWrite = dst->Write(buf.data(), got, &put);
            if (FAILED(hrWrite)) return hrWrite;
            totalRead += got;
            totalWritten += put;
            if (put != got) break;
        }
        if (read) read->QuadPart = totalRead;
        if (written) written->QuadPart = totalWritten;
        return S_OK;
    }
    STDMETHODIMP Commit(DWORD) override { return S_OK; }
    STDMETHODIMP Revert() override { return E_NOTIMPL; }
    STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    STDMETHODIMP Stat(STATSTG *st, DWORD) override
    {
        if (!st) return STG_E_INVALIDPOINTER;
        ZeroMemory(st, sizeof(*st));
        st->type = STGTY_STREAM;
        st->cbSize.QuadPart = _size;
        st->grfMode = STGM_READ;
        return S_OK;
    }
    STDMETHODIMP Clone(IStream **) override { return E_NOTIMPL; }

private:
    ~CRemoteStream()
    {
        if (_h != INVALID_HANDLE_VALUE) CloseHandle(_h);
        if (!_local.empty()) DeleteFileW(_local.c_str());
    }

    BOOL Ensure()
    {
        if (_done) return _h != INVALID_HANDLE_VALUE;
        if (_cancelled || _cancelRequested) return FALSE; // cancellation is terminal; never retry it
        _done = TRUE;
        _fetchStarted = TRUE;
        WCHAR tmp[MAX_PATH] = {}, dir[MAX_PATH] = {};
        if (!GetTempPathW(ARRAYSIZE(tmp), tmp)) return FALSE;
        StringCchCopyW(dir, ARRAYSIZE(dir), tmp);
        StringCchCatW(dir, ARRAYSIZE(dir), L"rfs-dataobj");
        CreateDirectoryW(dir, NULL);
        static LONG s_seq = 0;
        LONG seq = InterlockedIncrement(&s_seq);
        WCHAR name[MAX_PATH] = {};
        StringCchPrintfW(name, ARRAYSIZE(name), L"%s\\%u_%ld_%s", dir, GetCurrentProcessId(), seq,
                         PathFindFileNameW(_remote.c_str()));
        _local = name;
        DeleteFileW(_local.c_str());
        ULARGE_INTEGER freeBytes = {};
        BOOL hasFreeBytes = GetDiskFreeSpaceExW(dir, &freeBytes, NULL, NULL);
        ProbeLog(L"[DL] Ensure start tid=%lu site='%s' remote='%s' size=%llu free=%llu has_free=%d -> '%s' batch='%s'",
                 GetCurrentThreadId(), _site.c_str(), _remote.c_str(), (unsigned long long)_size,
                 (unsigned long long)freeBytes.QuadPart, (int)hasFreeBytes, _local.c_str(), _batchId.c_str());
        const ULONGLONG tEnsure = GetTickCount64();

        // 边下边读：只**发起** FETCH（不等它下完），等本地文件开始有数据就返回；
        // 之后 Read() 再按需等待更多字节。这样第一次 Read 不再被"整个文件下完"卡住
        // —— 之前实测 6 GB 文件在调用线程上干等 10.5 s，就是这里。
        if (!FtpBridgeFetchStart(L"FETCH", _site.c_str(), _remote.c_str(), _local.c_str(), _batchId.c_str(), _jobId))
        {
            ProbeLog(L"[DL] Ensure FAILED: cannot start fetch site='%s' remote='%s' batch='%s'",
                     _site.c_str(), _remote.c_str(), _batchId.c_str());
            DeleteFileW(_local.c_str());
            _local.clear();
            _done = FALSE;
            return FALSE;
        }
        _jobDone = FALSE;
        _jobFailed = FALSE;
        ProbeLog(L"[DL] ensure started job=%hs remote='%s' batch='%s'", _jobId.c_str(), _remote.c_str(), _batchId.c_str());

        const ULONGLONG deadline = tEnsure + 120000;   // 最多等 2 分钟开始出数据
        for (;;)
        {
            WIN32_FILE_ATTRIBUTE_DATA fa = {};
            if (GetFileAttributesExW(_local.c_str(), GetFileExInfoStandard, &fa)) break;
            if (PollJobTerminal()) break;
            if (GetTickCount64() > deadline) break;
            Sleep(50);
        }
        // 打开本地临时文件：允许共享读/写/删（写方可能还在写、失败时会被删）。
        // 若写方以独占方式打开（例如 FTP 后端），会持续 ERROR_SHARING_VIOLATION(32)：
        // 这时**不立刻失败**，等它写完再开 —— 该路退化成"下完再读"，但不会误报磁盘错误。
        const ULONGLONG openDeadline = GetTickCount64() + 120000;
        for (;;)
        {
            _h = CreateFileW(_local.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
            if (_h != INVALID_HANDLE_VALUE) break;
            DWORD err = GetLastError();
            if (err != ERROR_SHARING_VIOLATION && err != ERROR_LOCK_VIOLATION)
            {
                ProbeLog(L"[DL] Ensure FAILED: cannot open local '%s' err=%lu cancelled=%d failed=%d",
                         _local.c_str(), (unsigned long)err, (int)_cancelled, (int)_jobFailed);
                DeleteFileW(_local.c_str());
                _local.clear();
                if (!_cancelled) _done = FALSE;    // 普通失败可重试；取消是终态
                return FALSE;
            }
            if (GetTickCount64() > openDeadline)
            {
                ProbeLog(L"[DL] Ensure FAILED: open timed out (sharing) '%s'", _local.c_str());
                DeleteFileW(_local.c_str());
                _local.clear();
                if (!_cancelled) _done = FALSE;
                return FALSE;
            }
            PollJobTerminal();   // 顺便刷新终态（失败/取消时 Read 据此收尾）
            Sleep(50);
        }
        _downloadReady = TRUE;
        ProbeLog(L"[DL] ensure opened tid=%lu elapsedMs=%llu local='%s' size_on_disk=%llu expected=%llu",
                 GetCurrentThreadId(), GetTickCount64() - tEnsure, _local.c_str(),
                 (unsigned long long)OnDiskSize(), (unsigned long long)_size);
        return TRUE;
    }

    // 本地临时文件当前已落盘多少字节（边下边长）
    ULONGLONG OnDiskSize()
    {
        if (_h == INVALID_HANDLE_VALUE) return 0;
        LARGE_INTEGER li = {};
        if (!GetFileSizeEx(_h, &li)) return 0;
        return (ULONGLONG)li.QuadPart;
    }

    // 查询服务端任务是否已终态。返回 TRUE = 已终态（_jobDone 置位；失败/取消另标记）。
    BOOL PollJobTerminal()
    {
        if (_jobId.empty() || _jobDone) return TRUE;
        std::wstring w(_jobId.begin(), _jobId.end());
        std::string st;
        if (!FtpBridgeFetchStatus(w.c_str(), st)) return FALSE;   // 短查询失败：当作还在跑
        if (st.rfind("DONE", 0) == 0) { _jobDone = TRUE; return TRUE; }
        if (st.rfind("CANCELLED", 0) == 0) { _jobDone = TRUE; _cancelled = TRUE; return TRUE; }
        if (st.rfind("FAILED", 0) == 0) { _jobDone = TRUE; _jobFailed = TRUE; return TRUE; }
        if (st.rfind("UNKNOWN", 0) == 0) { _jobDone = TRUE; _jobFailed = TRUE; return TRUE; }
        return FALSE;
    }

    LONG _ref;
    std::wstring _site, _remote, _local;
    std::wstring _batchId;
    ULONGLONG _size, _pos;
    HANDLE _h;
    BOOL _done;                 // Ensure has begun; not equivalent to a completed download
    BOOL _fetchStarted;         // a FETCH job exists and can be cancelled by stream release
    BOOL _downloadReady;        // local file opened successfully
    BOOL _cancelled;            // explicit CANCELLED terminal state from FETCHSTATUS
    BOOL _cancelRequested;      // stream-release cancellation was sent to the service
    BOOL _loggedFirstRead = FALSE;
    // 边下边读（2026-09-20）：FETCH 只发起、不等完成；Read 按需等字节。
    std::string _jobId;
    BOOL _jobDone = FALSE;
    BOOL _jobFailed = FALSE;
    int _readCount = 0;
};

// ---------------------------------------------------------------------------
// IShellItemResources（{ff5693be-2ce0-4d48-b5c5-40817d1acdb9}）
//
// Shell 的复制引擎经 ITransferSource::OpenItem 取源项时，**先**要这个接口拿
// 属性/大小/时间/资源描述，之后才要 IStream。不实现它就直接 E_NOINTERFACE
// （用户看到的就是"0x80004002 不支持的接口"）。
//
// 这里给出的都是我们已知的元数据（缓存里的 size/mtime + 名字），资源方面只支持
// 默认资源 —— 也就是文件内容流本身（OpenResource(IID_IStream)）。
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 内容资源 GUID：由提供方自定义（SDK 没有预定义常量）。Shell 会**按我们枚举的**
// 资源来取内容，所以枚举一个"数据资源"即可。实测枚举为空时 Shell 会直接收工
// （既不来要 IStream，也不报错）—— 那正是"只传了占位、没有下载"的原因。
// {8f2a1c66-7b0d-4a5e-9d3f-6c1b2e4a5f70}
// ---------------------------------------------------------------------------
static const GUID ERF_GUID_DataResource =
{ 0x8f2a1c66, 0x7b0d, 0x4a5e, { 0x9d, 0x3f, 0x6c, 0x1b, 0x2e, 0x4a, 0x5f, 0x70 } };

// ---------------------------------------------------------------------------
// IEnumResources：枚举本项的**一个**内容资源（文件数据流本身）。
// ---------------------------------------------------------------------------
class CShellItemResourceEnum : public IEnumResources
{
public:
    CShellItemResourceEnum(const GUID &guid, PCWSTR name) : _ref(1), _guid(guid), _index(0)
    {
        if (name) StringCchCopyW(_name, ARRAYSIZE(_name), name);
        DllAddRef();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, ERF_IID_IEnumResources))
        {
            *ppv = static_cast<IEnumResources *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG n = InterlockedDecrement(&_ref);
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP Next(ULONG celt, SHELL_ITEM_RESOURCE *psir, ULONG *pceltFetched) override
    {
        ULONG n = 0;
        while (n < celt && _index < 1 && psir)
        {
            psir[n].guidType = _guid;
            StringCchCopyW(psir[n].szName, ARRAYSIZE(psir[n].szName), _name);
            ++n;
            ++_index;
        }
        if (pceltFetched) *pceltFetched = n;
        return (n == celt) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG celt) override { _index = min(1, _index + (LONG)celt); return S_OK; }
    STDMETHODIMP Reset() override { _index = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumResources **ppenumr) override
    {
        if (!ppenumr) return E_POINTER;
        CShellItemResourceEnum *e = new (std::nothrow) CShellItemResourceEnum(_guid, _name);
        if (!e) { *ppenumr = NULL; return E_OUTOFMEMORY; }
        e->_index = _index;
        *ppenumr = e;
        return S_OK;
    }

private:
    ~CShellItemResourceEnum() { DllRelease(); }
    LONG _ref;
    GUID _guid;
    LONG _index;
    WCHAR _name[260] = {};
};

class CRemoteItemResources : public IShellItemResources
{
public:
    CRemoteItemResources(PCWSTR site, PCWSTR remote, PCWSTR name, ULONGLONG size,
                         BOOL isFolder, DWORD mtimeUnix, PCWSTR batchId, const GUID &resourceGuid)
        : _ref(1), _site(site ? site : L""), _remote(remote ? remote : L""),
          _name(name ? name : L""), _size(size), _isFolder(isFolder),
          _mtimeUnix(mtimeUnix), _batchId(batchId ? batchId : L""), _resourceGuid(resourceGuid)
    {
        DllAddRef();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, ERF_IID_IShellItemResources))
        {
            *ppv = static_cast<IShellItemResources *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG n = InterlockedDecrement(&_ref);
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP GetAttributes(DWORD *pdwAttributes) override
    {
        ProbeLog(L"[XFER] IShellItemResources::GetAttributes tid=%lu '%s' folder=%d",
                 GetCurrentThreadId(), _name.c_str(), (int)_isFolder);
        if (!pdwAttributes) return E_POINTER;
        *pdwAttributes = _isFolder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        return S_OK;
    }
    STDMETHODIMP GetSize(ULONGLONG *pullSize) override
    {
        ProbeLog(L"[XFER] IShellItemResources::GetSize tid=%lu '%s' size=%llu",
                 GetCurrentThreadId(), _name.c_str(), _size);
        if (!pullSize) return E_POINTER;
        *pullSize = _size;
        return S_OK;
    }
    STDMETHODIMP GetTimes(FILETIME *pftCreation, FILETIME *pftWrite, FILETIME *pftAccess) override
    {
        ULARGE_INTEGER li;
        li.QuadPart = (ULONGLONG)_mtimeUnix * 10000000ULL + 116444736000000000ULL;
        if (pftCreation) { pftCreation->dwLowDateTime = li.LowPart; pftCreation->dwHighDateTime = li.HighPart; }
        if (pftWrite)    { pftWrite->dwLowDateTime = li.LowPart;    pftWrite->dwHighDateTime = li.HighPart; }
        if (pftAccess)   { pftAccess->dwLowDateTime = li.LowPart;   pftAccess->dwHighDateTime = li.HighPart; }
        return S_OK;
    }
    STDMETHODIMP SetTimes(const FILETIME *, const FILETIME *, const FILETIME *) override
    {
        ProbeLog(L"[XFER] IShellItemResources::SetTimes (no-op) '%s'", _name.c_str());
        return S_OK;   // 源项时间不回写；返回 E_NOTIMPL 会让引擎中止
    }
    STDMETHODIMP GetResourceDescription(const SHELL_ITEM_RESOURCE *pcsir, LPWSTR *ppszDescription) override
    {
        ProbeLog(L"[XFER] IShellItemResources::GetResourceDescription '%s' guidType=%08X", _name.c_str(), pcsir ? pcsir->guidType.Data1 : 0);
        if (!ppszDescription) return E_POINTER;
        return SHStrDupW(_name.c_str(), ppszDescription);
    }
    STDMETHODIMP EnumResources(IEnumResources **ppenumr) override
    {
        ProbeLog(L"[XFER] IShellItemResources::EnumResources '%s' -> 1 data resource guidType=%08X",
                 _name.c_str(), _resourceGuid.Data1);
        if (!ppenumr) return E_POINTER;
        *ppenumr = new (std::nothrow) CShellItemResourceEnum(_resourceGuid, _name.c_str());
        return *ppenumr ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP SupportsResource(const SHELL_ITEM_RESOURCE *pcsir) override
    {
        // 对**任何**资源都说支持：内容只有一个（文件数据流），Shell 用哪个 GUID 来问都给它。
        ProbeLog(L"[XFER] IShellItemResources::SupportsResource '%s' guidType=%08X -> S_OK", _name.c_str(), pcsir ? pcsir->guidType.Data1 : 0);
        return S_OK;
    }
    STDMETHODIMP OpenResource(const SHELL_ITEM_RESOURCE *pcsir, REFIID riid, void **ppv) override
    {
        ProbeLog(L"[XFER] IShellItemResources::OpenResource tid=%lu '%s' guidType=%08X riid=%08X",
                 GetCurrentThreadId(), _name.c_str(), pcsir ? pcsir->guidType.Data1 : 0, riid.Data1);
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (_isFolder) return E_NOINTERFACE;
        if (!IsEqualIID(riid, IID_IStream) && !IsEqualIID(riid, IID_ISequentialStream)) return E_NOINTERFACE;
        CRemoteStream *stream = new (std::nothrow) CRemoteStream(_site.c_str(), _remote.c_str(), _size, _batchId.c_str());
        if (!stream) return E_OUTOFMEMORY;
        HRESULT hr = stream->QueryInterface(riid, ppv);
        stream->Release();
        return hr;
    }
    STDMETHODIMP CreateResource(const SHELL_ITEM_RESOURCE *, REFIID riid, void **) override
    {
        ProbeLog(L"[XFER] IShellItemResources::CreateResource '%s' riid=%08X -> E_NOTIMPL", _name.c_str(), riid.Data1);
        return E_NOTIMPL;   // 源端不创建资源（我们只读远程）
    }
    STDMETHODIMP MarkForDelete() override
    {
        ProbeLog(L"[XFER] IShellItemResources::MarkForDelete (no-op) '%s'", _name.c_str());
        return S_OK;   // 源端"标记删除"我们不做；返回 E_NOTIMPL 会让引擎中止
    }

private:
    ~CRemoteItemResources() { DllRelease(); }

    LONG _ref;
    std::wstring _site, _remote, _name, _batchId;
    ULONGLONG _size;
    BOOL _isFolder;
    DWORD _mtimeUnix;
    GUID _resourceGuid;
};

// ---------------------------------------------------------------------------
// Folder fetch: 整个目录树的下载**交给常驻服务**（一个文件夹 = 一个队列任务），
// 下载到本地临时根目录，流再从本地树里读。
//
// 以前这里 spawn 一个 `cli getr` 进程：Shell DLL 自己启动 CLI，违反架构约束
//（会话/连接/遍历/进度/取消都归常驻服务），而且每个文件夹一个进程、一次全新登录。
// 现在只发一个 FETCHDIR 请求，服务侧扫树 + 逐个下载 + 进度聚合，桥接边界等最终回执。
//
// 引用计数：数据对象与每个存活流各持一份；归零时删掉临时树（没有进程要杀了）。
// ---------------------------------------------------------------------------
class CFolderFetch
{
public:
    CFolderFetch(PCWSTR site, PCWSTR remoteDir)
        : _ref(1), _started(FALSE), _finished(FALSE), _cancelled(FALSE), _seq(0)
    {
        if (site) _site = site;
        if (remoteDir) _remoteDir = remoteDir;
        WCHAR tmp[MAX_PATH] = {};
        if (GetTempPathW(ARRAYSIZE(tmp), tmp))
        {
            _seq = InterlockedIncrement(&s_seq);
            WCHAR buf[MAX_PATH] = {};
            StringCchPrintfW(buf, ARRAYSIZE(buf), L"%srfs-copy-%u-%ld", tmp, GetCurrentProcessId(), _seq);
            _localRoot = buf;
            CreateDirectoryW(_localRoot.c_str(), NULL);
        }
        // 一个文件夹 = 一个批次 id：队列窗口据此把它显示成**一个**任务；
        // 同时拖多个文件夹时，每个文件夹各自一组（用户明确要的语义）。
        StringCchPrintfW(_batchId, ARRAYSIZE(_batchId), L"dir-%u-%ld",
                         (unsigned)GetCurrentProcessId(), _seq);
        InitializeCriticalSection(&_cs);
        DllAddRef();   // 2026-09-20: the fetch thread below outlives every COM ref
    }

    ~CFolderFetch()
    {
        // 没有进程要杀了：下载跑在常驻服务里，取消走队列窗口的「取消」按钮。
        DeleteTree(_localRoot);
        DeleteCriticalSection(&_cs);
        DllRelease();
    }

    LONG AddRef() { return InterlockedIncrement(&_ref); }
    LONG Release()
    {
        LONG n = InterlockedDecrement(&_ref);
        if (n == 0) delete this;
        return n;
    }

    void EnsureStarted()
    {
        EnterCriticalSection(&_cs);
        if (_started) { LeaveCriticalSection(&_cs); return; }
        _started = TRUE;
        LeaveCriticalSection(&_cs);

        // 后台线程发请求并等回执：调用点（GetData）不能在这里同步等整棵树下完。
        AddRef();      // 线程持有一份引用，直到下载结束
        HANDLE th = CreateThread(NULL, 0, &CFolderFetch::ThreadProc, this, 0, NULL);
        if (th)
        {
            CloseHandle(th);
        }
        else
        {
            _finished = TRUE;
            Release();
        }
    }

    static DWORD WINAPI ThreadProc(LPVOID param)
    {
        CFolderFetch *self = (CFolderFetch *)param;
        self->RunFetch();
        self->Release();
        return 0;
    }

    // 下载交给常驻服务：Shell DLL 不再 spawn `cli getr` —— 会话复用、遍历、进度、
    // 取消都归常驻服务，桥接边界只等它的最终 OK/FAIL。
    void RunFetch()
    {
        if (_localRoot.empty())
        {
            ProbeLog(L"[DATAOBJ] fetchdir cannot start (local root empty)");
            _finished = TRUE;
            return;
        }
        std::string reply;
        FtpBridgeFetchState state = FtpBridgeFetchDir(_site.c_str(), _remoteDir.c_str(), _localRoot.c_str(), _batchId, reply);
        if (state == FtpBridgeFetchState::Cancelled) _cancelled = TRUE;
        _finished = TRUE;
        ProbeLog(L"[DATAOBJ] fetchdir(service) site='%s' dir='%s' -> '%s' batch='%s' state=%d",
                 _site.c_str(), _remoteDir.c_str(), _localRoot.c_str(), _batchId, (int)state);
    }
    std::wstring LocalPath(const std::wstring &rel) const
    {
        std::wstring p = _localRoot;
        p += L"\\";
        p += rel;       // rel already uses backslash separators
        return p;
    }

    // 等文件出现。服务回执之后（_finished）再给最后一次机会，然后如实返回失败 ——
    // 以前是靠"getr 进程退出了"判断，现在没有进程了，改看服务回执。
    BOOL Cancelled() const { return _cancelled; }   // 服务回话说用户取消了（终态）
    BOOL WaitForFile(const std::wstring &rel)
    {
        std::wstring p = LocalPath(rel);
        const ULONGLONG t0 = GetTickCount64();
        for (;;)
        {
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                ULONGLONG dt = GetTickCount64() - t0;
                if (dt >= 500)   // 谁在等、等了多久：确认不是 UI 线程在等
                    ProbeLog(L"[DATAOBJ] WaitForFile slow tid=%lu elapsedMs=%llu '%s'",
                             GetCurrentThreadId(), dt, rel.c_str());
                return TRUE;
            }
            if (_finished)
            {
                for (int i = 0; i < 5; i++)
                {
                    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return TRUE;
                    Sleep(80);
                }
                return FALSE;
            }
            Sleep(80);
        }
    }

private:
    static LONG s_seq;

    static void DeleteTree(const std::wstring &path)
    {
        if (path.empty()) return;
        // Double-null-terminated path for SHFileOperation.
        int n = (int)path.size();
        std::vector<WCHAR> buf(n + 2, 0);
        memcpy(buf.data(), path.c_str(), (size_t)n * sizeof(WCHAR));
        buf[n] = 0; buf[n + 1] = 0;
        SHFILEOPSTRUCTW op = {};
        op.wFunc = FO_DELETE;
        op.pFrom = buf.data();
        op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOF_ALLOWUNDO;
        SHFileOperationW(&op);
    }

    LONG _ref;
    std::wstring _site, _remoteDir, _localRoot;
    WCHAR _batchId[48] = {};      // 一个文件夹一个批次 id（队列里一个任务）
    volatile BOOL _started;
    volatile BOOL _finished;      // 服务已回执（下载结束）
    volatile BOOL _cancelled;     // 服务回话说用户取消了
    LONG _seq;
    CRITICAL_SECTION _cs;
};
LONG CFolderFetch::s_seq = 0;

// ---------------------------------------------------------------------------
// Local-file stream: serves bytes from a file the 'getr' process already
// wrote. Holds a ref on the owning fetch so the temp tree stays alive while the
// shell reads.
// ---------------------------------------------------------------------------
class CLocalStream : public IStream
{
public:
    CLocalStream(PCWSTR localPath, ULONGLONG size, CFolderFetch *fetch)
        : _ref(1), _size(size), _pos(0), _h(INVALID_HANDLE_VALUE), _fetch(fetch)
    {
        if (localPath) _local = localPath;
        _h = CreateFileW(_local.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (_fetch) _fetch->AddRef();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IStream) ||
            IsEqualIID(riid, IID_ISequentialStream))
        {
            *ppv = static_cast<IStream *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG n = InterlockedDecrement(&_ref);
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP Read(void *pv, ULONG cb, ULONG *pcbRead) override
    {
        if (pcbRead) *pcbRead = 0;
        if (!pv) return STG_E_INVALIDPOINTER;
        if (_h == INVALID_HANDLE_VALUE) return STG_E_READFAULT;
        DWORD got = 0;
        if (!ReadFile(_h, pv, cb, &got, NULL)) return STG_E_READFAULT;
        if (pcbRead) *pcbRead = got;
        _pos += got;
        return got == 0 ? S_FALSE : S_OK;
    }
    STDMETHODIMP Write(const void *, ULONG, ULONG *) override { return STG_E_ACCESSDENIED; }
    STDMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *newPos) override
    {
        if (_h == INVALID_HANDLE_VALUE) return STG_E_READFAULT;
        LARGE_INTEGER zero = {};
        if (!SetFilePointerEx(_h, move, &zero, origin)) return STG_E_INVALIDFUNCTION;
        _pos = (ULONGLONG)zero.QuadPart;
        if (newPos) newPos->QuadPart = _pos;
        return S_OK;
    }
    STDMETHODIMP SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    STDMETHODIMP CopyTo(IStream *dst, ULARGE_INTEGER cb, ULARGE_INTEGER *rd, ULARGE_INTEGER *wr) override
    {
        if (!dst) return STG_E_INVALIDPOINTER;
        std::vector<BYTE> buf(64 * 1024);
        ULONGLONG tr = 0, tw = 0;
        while (tr < cb.QuadPart)
        {
            ULONGLONG want64 = (ULONGLONG)buf.size();
            if (want64 > cb.QuadPart - tr) want64 = cb.QuadPart - tr;
            ULONG want = (ULONG)want64, got = 0;
            if (FAILED(Read(buf.data(), want, &got)) || got == 0) break;
            ULONG put = 0;
            if (FAILED(dst->Write(buf.data(), got, &put))) break;
            tr += got; tw += put;
            if (put != got) break;
        }
        if (rd) rd->QuadPart = tr;
        if (wr) wr->QuadPart = tw;
        return S_OK;
    }
    STDMETHODIMP Commit(DWORD) override { return S_OK; }
    STDMETHODIMP Revert() override { return E_NOTIMPL; }
    STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    STDMETHODIMP Stat(STATSTG *st, DWORD) override
    {
        if (!st) return STG_E_INVALIDPOINTER;
        ZeroMemory(st, sizeof(*st));
        st->type = STGTY_STREAM;
        st->cbSize.QuadPart = _size;
        st->grfMode = STGM_READ;
        return S_OK;
    }
    STDMETHODIMP Clone(IStream **) override { return E_NOTIMPL; }

private:
    ~CLocalStream()
    {
        if (_h != INVALID_HANDLE_VALUE) CloseHandle(_h);
        if (_fetch) _fetch->Release();
    }

    LONG _ref;
    std::wstring _local;
    ULONGLONG _size, _pos;
    HANDLE _h;
    CFolderFetch *_fetch;
};

// ---------------------------------------------------------------------------
// Data object: descriptors up front, contents lazily.
// ---------------------------------------------------------------------------
class CRemoteDataObject : public IDataObject, public IDataObjectAsyncCapability
{
public:
    struct Item
    {
        std::wstring site, folder, name;
        std::wstring relPath;      // path INSIDE the copied tree ("dir\\sub\\f.txt")
        ULONGLONG size;
        DWORD mtime;
        BOOL isFolder;
        CFolderFetch *fetch = nullptr;   // non-null for items inside a top-level folder
    };

    // Hard ceiling on the flattened tree we publish. FILEDESCRIPTORW is ~592 B,
    // so 100 000 entries cost one ~59 MB movable HGLOBAL -- acceptable for a
    // clipboard payload, and it is what makes the lab's 100 000-file folders copy
    // COMPLETELY (the old 5000 silently truncated them; see docs/
    // UI_THREAD_FREEZE_AND_DATAOBJECT_2026-09-14.md §6.1). Exceeding the ceiling
    // now REFUSES the formats instead of publishing a partial tree.
    static const size_t kMaxItems = 200000;
    // How long after being born inside a probe window a query still counts as the
    // shell's own follow-up probing rather than a user-initiated transfer.
    static const DWORD kProbeGraceMs = 1500;

    static HRESULT Create(REFIID riid, void **ppv)
    {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        CRemoteDataObject *obj = new (std::nothrow) CRemoteDataObject();
        if (!obj) return E_OUTOFMEMORY;
        HRESULT hr = obj->QueryInterface(riid, ppv);
        obj->Release();
        return hr;
    }

    void Add(PCWSTR site, PCWSTR folder, PCWSTR name, ULONGLONG size, DWORD mtime, BOOL isFolder)
    {
        Item it;
        it.site = site ? site : L"";
        it.folder = folder ? folder : L"";
        it.name = name ? name : L"";
        it.relPath = it.name;
        it.size = size;
        it.mtime = mtime;
        it.isFolder = isFolder;
        _tops.push_back(it);
    }

    // Born inside a shell probe window (default context menu / command-bar verbs
    // / drag preparation). While that probe is live this object must never block
    // the UI thread -- but it may well be the very object that ends up on the
    // clipboard, so afterwards it has to answer for real.
    void SuppressFetch() { _probeBorn = TRUE; _probeTick = GetTickCount(); }

    // 数据对象刚建立（Ctrl+C / 拖拽开始 / 菜单探测）时，把选中的文件夹子树放到后台
    // 列表预热（FtpPrefetchTreeQuiet）。理由：真正粘贴时 Shell 会在**发起窗口的 UI
    // 线程**上查询 CFSTR_FILEDESCRIPTORW 并同步枚举整棵树；预热把这一步变成缓存命中，
    // 不再让用户看到窗口假死（实测冷目录 26535 项一次同步 LIST = 2750 ms）。
    void Prewarm()
    {
        if (_prewarmStarted) return;
        _prewarmStarted = TRUE;
        for (size_t i = 0; i < _tops.size(); i++)
        {
            if (!_tops[i].isFolder) continue;
            std::wstring full = _tops[i].folder;
            if (!full.empty() && full[full.size() - 1] != L'/') full += L'/';
            full += _tops[i].name;
            FtpPrefetchTreeQuiet(_tops[i].site.c_str(), full.c_str());
        }
    }

    // Still being probed? True inside the guard window, and for a short grace
    // period after it: the shell's own follow-up query lands milliseconds after
    // SHCreateDefaultContextMenu returned (measured: the drag-preparation GetData
    // came 78 ms after [MENU] exit). Blocking there is the "右键/删除 转圈几秒"
    // freeze; a real copy or paste asks much later, once the user clicked.
    BOOL BeingProbed() const
    {
        if (UiFetchSuppressed()) return TRUE;
        if (!_probeBorn) return FALSE;
        return (DWORD)(GetTickCount() - _probeTick) < kProbeGraceMs;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDataObject))
        {
            *ppv = static_cast<IDataObject *>(this);
            AddRef();
            return S_OK;
        }
        if (IsEqualIID(riid, ERF_IID_IDataObjectAsyncCapability))
        {
            *ppv = static_cast<IDataObjectAsyncCapability *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    // ---- IDataObjectAsyncCapability ---------------------------------------
    // 只要 Shell 问起/设置这个接口，就留一条日志 —— 这是判断 A 方案在 Win11 上
    // 到底有没有被采纳的唯一证据（如果一直没有任何 [ASYNC] 行，说明复合数据对象
    // 没把接口透出去，得走 B）。
    STDMETHODIMP SetAsyncMode(BOOL fDoOpAsync) override
    {
        ProbeLog(L"[ASYNC] SetAsyncMode %d tid=%lu", (int)fDoOpAsync, GetCurrentThreadId());
        _asyncMode = fDoOpAsync;
        return S_OK;
    }
    STDMETHODIMP GetAsyncMode(BOOL *pfIsOpAsync) override
    {
        if (!pfIsOpAsync) return E_POINTER;
        *pfIsOpAsync = _asyncMode;
        ProbeLog(L"[ASYNC] GetAsyncMode -> %d tid=%lu", (int)_asyncMode, GetCurrentThreadId());
        return S_OK;
    }
    STDMETHODIMP StartOperation(IBindCtx * /*pbcReserved*/) override
    {
        ProbeLog(L"[ASYNC] StartOperation tid=%lu", GetCurrentThreadId());
        _inOperation = TRUE;
        // 越早开始后台预热越好：即使随后那次 GetData 仍在调用线程上同步执行，
        // 也能直接吃缓存，而不是自己再拉一遍网络。
        Prewarm();
        return S_OK;
    }
    STDMETHODIMP InOperation(BOOL *pfInAsyncOp) override
    {
        if (!pfInAsyncOp) return E_POINTER;
        *pfInAsyncOp = _inOperation;
        return S_OK;
    }
    STDMETHODIMP EndOperation(HRESULT /*hResult*/, IBindCtx * /*pbcReserved*/, DWORD /*dwEffects*/) override
    {
        ProbeLog(L"[ASYNC] EndOperation tid=%lu", GetCurrentThreadId());
        _inOperation = FALSE;
        _asyncMode = FALSE;
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG n = InterlockedDecrement(&_ref);
        if (n == 0) delete this;
        return n;
    }

    // Flatten the selected tree. cacheOnly=TRUE answers from the in-memory /
    // on-disk listing cache only and is instant; cacheOnly=FALSE is allowed to
    // pay for one synchronous listing fetch. Returns FALSE when some directory
    // could not be listed -- the caller must then REFUSE the virtual-file
    // formats, never publish a half-built tree (Explorer would create a folder
    // that is silently missing files, or hand the copy engine a list whose
    // indices no longer mean anything).
    BOOL TryExpand(BOOL cacheOnly)
    {
        std::vector<Item> items;
        std::vector<CFolderFetch *> fetches;
        BOOL cold = FALSE;
        // 2026-09-20 探针：量化"源端复制冻结"——展开发生在哪个线程、遍历多少目录、
        // 其中多少目录必须走同步网络列举、总共耗时多久（对比 UI 线程 id 即可判断）。
        const ULONGLONG t0 = GetTickCount64();
        UINT dirs = 0, coldDirs = 0;
        for (size_t i = 0; i < _tops.size(); i++)
        {
            CFolderFetch *fetch = nullptr;
            if (_tops[i].isFolder)
            {
                std::wstring full = _tops[i].folder;
                if (!full.empty() && full[full.size() - 1] != L'/') full += L'/';
                full += _tops[i].name;
                fetch = new (std::nothrow) CFolderFetch(_tops[i].site.c_str(), full.c_str());
                if (fetch) fetches.push_back(fetch);
            }
            ExpandInto(items, cold, _tops[i], fetch, cacheOnly, dirs, coldDirs);
        }
        if (cold)
        {
            // The fetch contexts belong to a tree we are throwing away: clear the
            // pointers before releasing, so nothing can dereference them later.
            for (size_t i = 0; i < items.size(); i++) items[i].fetch = nullptr;
            for (size_t i = 0; i < fetches.size(); i++) fetches[i]->Release();
            ProbeLog(L"[DATAOBJ] expand COLD cacheOnly=%d tid=%lu dirs=%u coldDirs=%u items=%u elapsedMs=%llu",
                     (int)cacheOnly, GetCurrentThreadId(), dirs, coldDirs, (UINT)items.size(),
                     GetTickCount64() - t0);
            return FALSE;
        }
        if (items.size() >= kMaxItems)
        {
            // Hitting the cap means the tree we would publish is MISSING entries.
            // Publishing it is exactly how a copy silently loses files: measured
            // with the old 5000 limit, a 100 000-file folder produced precisely
            // 5000 GetData contents requests, one "capped" line -- and a SUCCESS
            // report. Refuse instead, so the user gets an error rather than a
            // half-populated destination folder.
            for (size_t i = 0; i < items.size(); i++) items[i].fetch = nullptr;
            for (size_t i = 0; i < fetches.size(); i++) fetches[i]->Release();
            ProbeLog(L"[DATAOBJ] tree hits cap=%u -> refuse (would silently copy a partial tree)",
                     (UINT)kMaxItems);
            ProbeLog(L"[DATAOBJ] expand CAP cacheOnly=%d tid=%lu dirs=%u coldDirs=%u items=%u elapsedMs=%llu",
                     (int)cacheOnly, GetCurrentThreadId(), dirs, coldDirs, (UINT)items.size(),
                     GetTickCount64() - t0);
            return FALSE;
        }
        _items.swap(items);
        for (size_t i = 0; i < fetches.size(); i++) _fetches.push_back(fetches[i]);
        _expanded = TRUE;
        ProbeLog(L"[DATAOBJ] expand done cacheOnly=%d tid=%lu tops=%u dirs=%u coldDirs=%u items=%u elapsedMs=%llu",
                 (int)cacheOnly, GetCurrentThreadId(), (UINT)_tops.size(), dirs, coldDirs,
                 (UINT)_items.size(), GetTickCount64() - t0);
        return TRUE;
    }

    void ExpandIfNeeded()
    {
        if (_expanded) return;
        if (TryExpand(TRUE)) return;             // warm: answered without blocking

        // 冷路径：把线程与 async 状态记下来 —— 判断 Shell 是否把这次展开放在了
        // 后台线程（async=1 且 tid 与窗口线程不同）还是仍在窗口线程上。
        ProbeLog(L"[DATAOBJ] ExpandIfNeeded cold tid=%lu async=%d inOp=%d suppressed=%d",
                 GetCurrentThreadId(), (int)_asyncMode, (int)_inOperation, (int)BeingProbed());

        if (BeingProbed())
        {
            // The shell is only asking "what is selected" while it builds the
            // default context menu / command-bar verbs or prepares a drag. Those
            // callers also hold CFSTR_SHELLIDLIST from the outer object, so
            // refusing our two formats costs them nothing, and we never start the
            // multi-second listing fetch on the UI thread. The prefetch that
            // TryExpand started fills the cache; the next query answers for real.
            ProbeLog(L"[DATAOBJ] cold while probing -> refuse formats tops=%u", (UINT)_tops.size());
            return;                               // _expanded stays FALSE -> GetData refuses
        }

        // A real consumer (copy / paste / drop) is waiting for the file list.
        // Correctness beats an instant answer here, so pay for the fetch.
        if (TryExpand(FALSE)) return;
        ProbeLog(L"[DATAOBJ] listing fetch failed -> refuse formats tops=%u", (UINT)_tops.size());
    }

    void ExpandInto(std::vector<Item> &out, BOOL &cold, const Item &dir, CFolderFetch *fetch, BOOL cacheOnly,
                    UINT &dirs, UINT &coldDirs)
    {
        if (out.size() >= kMaxItems) return;     // capped, but still a valid tree
        Item it = dir;
        it.fetch = fetch;
        out.push_back(it);
        if (!dir.isFolder) return;
        ++dirs;

        std::wstring full = dir.folder;
        if (!full.empty() && full[full.size() - 1] != L'/') full += L'/';
        full += dir.name;

        std::vector<FTPENTRY> kids;
        BOOL have = cacheOnly ? FtpCachePeekAll(dir.site.c_str(), full.c_str(), kids)
                              : FtpListCachedAll(dir.site.c_str(), full.c_str(), kids, /*waitForWarm=*/true);
        if (!have)
        {
            cold = TRUE;
            ++coldDirs;
            if (cacheOnly) FtpPrefetchQuiet(dir.site.c_str(), full.c_str());
            ProbeLog(L"[DATAOBJ] %s '%s'", cacheOnly ? L"cold" : L"list failed", full.c_str());
            return;
        }
        for (size_t k = 0; k < kids.size(); k++)
        {
            if (out.size() >= kMaxItems) return;
            Item it2;
            it2.site = dir.site;
            it2.folder = full;
            it2.name = kids[k].szName;
            it2.relPath = dir.relPath + L"\\" + kids[k].szName;
            it2.size = kids[k].dwSize;
            it2.mtime = kids[k].dwMtime;
            it2.isFolder = kids[k].fIsFolder;
            it2.fetch = fetch;       // inherit the folder's fetch context
            ExpandInto(out, cold, it2, fetch, cacheOnly, dirs, coldDirs);
        }
    }

    STDMETHODIMP GetData(FORMATETC *fmt, STGMEDIUM *medium) override
    {
        if (!fmt || !medium) return E_INVALIDARG;
        ZeroMemory(medium, sizeof(*medium));
        CLIPFORMAT cfDesc = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
        CLIPFORMAT cfContents = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILECONTENTS);

        // Only the two formats we actually serve may trigger the tree walk.
        // ExpandIfNeeded used to run FIRST for ANY format, so an unrelated probe
        // (CF_HDROP / "Shell IDList Array", which we do not support at all) paid
        // for a full synchronous enumeration of every selected folder -- that is
        // where the multi-second UI-thread pipe fetch actually came from.
        const BOOL wantDesc = (fmt->cfFormat == cfDesc);
        const BOOL wantContents = (fmt->cfFormat == cfContents && fmt->lindex >= 0);
        if (!wantDesc && !wantContents) return DV_E_FORMATETC;
        // 默认不再提供虚拟文件格式（见 ErfUseVirtualFileFormats）：让 Explorer 的复制
        // 引擎改走 ITransferSource（Shell 工作线程 + 自带进度窗口）。否则
        // GetData(FILEDESCRIPTORW) 会同步展开整棵树、GetData(FILECONTENTS) 会阻塞等
        // 整个文件下完 —— 两者都压在调用线程上。
        if (!ErfUseVirtualFileFormats()) return DV_E_FORMATETC;

        // Probe only the descriptor query (lindex == -1); per-file contents
        // queries (lindex >= 0) would storm the log during a copy.
        if (fmt->lindex < 0)
            ProbeLog(L"[DATAOBJ] GetData enter fmt=0x%04X suppressed=%d tid=%lu",
                     (UINT)fmt->cfFormat, (int)BeingProbed(), GetCurrentThreadId());
        ExpandIfNeeded();
        if (!_expanded) return DV_E_FORMATETC;   // cold tree while the shell probes us

        if (wantDesc)
        {
            SIZE_T bytes = sizeof(FILEGROUPDESCRIPTORW) + (SIZE_T)(_items.size() ? _items.size() - 1 : 0) * sizeof(FILEDESCRIPTORW);
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
            if (!h) return E_OUTOFMEMORY;
            FILEGROUPDESCRIPTORW *fgd = (FILEGROUPDESCRIPTORW *)GlobalLock(h);
            if (!fgd) { GlobalFree(h); return E_OUTOFMEMORY; }
            fgd->cItems = (UINT)_items.size();
            for (size_t i = 0; i < _items.size(); i++)
            {
                FILEDESCRIPTORW &fd = fgd->fgd[i];
                if (_items[i].relPath.size() >= ARRAYSIZE(fd.cFileName))
                {
                    // FileGroupDescriptorW's cFileName is WCHAR[MAX_PATH]: the
                    // clipboard drop protocol physically cannot express a deeper
                    // relative path. Guarding here beats StringCchCopyW refusing to
                    // copy and handing Explorer an entry with an EMPTY name (silent
                    // junk instead of an error). Deep trees must go through our own
                    // transfer engine -- see docs/ERF_PROTOCOL_PLAN.md §8.
                    GlobalUnlock(h);
                    GlobalFree(h);
                    ProbeLog(L"[DATAOBJ] rel path '%s' (%u chars) exceeds FileGroupDescriptor limit -> refuse",
                             _items[i].relPath.c_str(), (UINT)_items[i].relPath.size());
                    return DV_E_FORMATETC;
                }
                fd.dwFlags = FD_FILESIZE | FD_ATTRIBUTES | FD_WRITESTIME;
                fd.nFileSizeLow = (DWORD)(_items[i].size & 0xFFFFFFFF);
                fd.nFileSizeHigh = (DWORD)(_items[i].size >> 32);
                fd.dwFileAttributes = _items[i].isFolder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
                ULARGE_INTEGER li;
                li.QuadPart = (ULONGLONG)_items[i].mtime * 10000000ULL + 116444736000000000ULL;
                fd.ftLastWriteTime.dwLowDateTime = li.LowPart;
                fd.ftLastWriteTime.dwHighDateTime = li.HighPart;
                StringCchCopyW(fd.cFileName, ARRAYSIZE(fd.cFileName), _items[i].relPath.c_str());
            }
            GlobalUnlock(h);
            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = h;
            medium->pUnkForRelease = NULL;
            ProbeLog(L"[DATAOBJ] GetData descriptors n=%u", (UINT)_items.size());
            return S_OK;
        }

        if (fmt->cfFormat == cfContents && fmt->lindex >= 0 && (size_t)fmt->lindex < _items.size())
        {
            const Item &it = _items[(size_t)fmt->lindex];
            if (it.isFolder) return DV_E_LINDEX;

            // File inside a copied folder: ONE 'getr' downloads the whole
            // tree; serve this file from the local temp tree once it lands.
            if (it.fetch)
            {
                it.fetch->EnsureStarted();
                if (!it.fetch->WaitForFile(it.relPath))
                {
                    if (it.fetch->Cancelled())
                    {
                        // 用户在队列里取消了这次复制：如实回 CANCELLED，让资源管理器
                        // 报"已取消"，而不是"移动文件或文件夹时出错"。
                        ProbeLog(L"[DATAOBJ] fetch cancelled -> ERROR_CANCELLED '%s'", it.relPath.c_str());
                        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
                    }
                    ProbeLog(L"[DATAOBJ] fetch wait failed '%s'", it.relPath.c_str());
                    return STG_E_READFAULT;
                }
                std::wstring local = it.fetch->LocalPath(it.relPath);
                CLocalStream *stream = new (std::nothrow) CLocalStream(local.c_str(), it.size, it.fetch);
                if (!stream) return E_OUTOFMEMORY;
                medium->tymed = TYMED_ISTREAM;
                medium->pstm = stream;
                medium->pUnkForRelease = NULL;
                ProbeLog(L"[DATAOBJ] GetData contents idx=%d '%s' tid=%lu (from folder fetch)", (int)fmt->lindex, it.name.c_str(), GetCurrentThreadId());
                return S_OK;
            }

            // 顶层单文件：下载交给常驻服务（会话复用 + 传输队列 + 可取消），
            // 不再"一个文件一个 CLI 进程"。
            std::wstring remote = it.folder;
            if (!remote.empty() && remote[remote.size() - 1] != L'/') remote += L'/';
            remote += it.name;
            CRemoteStream *stream = new (std::nothrow) CRemoteStream(it.site.c_str(), remote.c_str(), it.size, _batchId);
            if (!stream) return E_OUTOFMEMORY;
            medium->tymed = TYMED_ISTREAM;
            medium->pstm = stream;
            medium->pUnkForRelease = NULL;
            ProbeLog(L"[DATAOBJ] GetData contents idx=%d '%s' tid=%lu", (int)fmt->lindex, it.name.c_str(), GetCurrentThreadId());
            return S_OK;
        }

        return DV_E_FORMATETC;
    }
    STDMETHODIMP GetDataHere(FORMATETC *, STGMEDIUM *) override { return E_NOTIMPL; }
    STDMETHODIMP QueryGetData(FORMATETC *fmt) override
    {
        if (!fmt) return E_INVALIDARG;
        if (!ErfUseVirtualFileFormats()) return DV_E_FORMATETC;   // 走 ITransferSource（见 ErfUseVirtualFileFormats）
        CLIPFORMAT cfDesc = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
        CLIPFORMAT cfContents = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILECONTENTS);
        if (fmt->cfFormat == cfDesc && (fmt->tymed & TYMED_HGLOBAL)) return S_OK;
        if (fmt->cfFormat == cfContents && (fmt->tymed & TYMED_ISTREAM)) return S_OK;
        return DV_E_FORMATETC;
    }
    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC *, FORMATETC *out) override
    {
        if (out) out->ptd = NULL;
        return E_NOTIMPL;
    }
    STDMETHODIMP SetData(FORMATETC *, STGMEDIUM *, BOOL) override { return E_NOTIMPL; }

    STDMETHODIMP EnumFormatEtc(DWORD direction, IEnumFORMATETC **out) override
    {
        if (!out) return E_POINTER;
        *out = NULL;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        // 默认不提供虚拟文件格式：让复制走 ITransferSource（见 ErfUseVirtualFileFormats）。
        if (!ErfUseVirtualFileFormats())
        {
            ProbeLog(L"[DATAOBJ] EnumFormatEtc -> 不提供虚拟文件格式（走 ITransferSource）");
            return E_NOTIMPL;
        }
        FORMATETC fmts[2] = {};
        fmts[0].cfFormat = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
        fmts[0].ptd = NULL; fmts[0].dwAspect = DVASPECT_CONTENT; fmts[0].lindex = -1; fmts[0].tymed = TYMED_HGLOBAL;
        fmts[1].cfFormat = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILECONTENTS);
        fmts[1].ptd = NULL; fmts[1].dwAspect = DVASPECT_CONTENT; fmts[1].lindex = 0; fmts[1].tymed = TYMED_ISTREAM;
        return SHCreateStdEnumFmtEtc(2, fmts, out);
    }

    STDMETHODIMP DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override { return OLE_E_ADVISENOTSUPPORTED; }
    STDMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    STDMETHODIMP EnumDAdvise(IEnumSTATDATA **) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    CRemoteDataObject() : _ref(1)
    {
        // 一次复制操作 = 一个数据对象 = 一个批次 id。
        // 队列窗口按它把"同一次复制出来的多个文件"折叠成一个任务组
        //（用户要求：一次复制是一个任务，任务内每个文件可展开查看）。
        StringCchPrintfW(_batchId, ARRAYSIZE(_batchId), L"%u-%llu",
                         (unsigned)GetCurrentProcessId(), GetTickCount64());
    }
    ~CRemoteDataObject()
    {
        for (size_t i = 0; i < _fetches.size(); i++) _fetches[i]->Release();
    }

    LONG _ref;
    WCHAR _batchId[48] = {};        // 本次复制的批次 id（见构造函数）
    std::vector<Item> _tops;        // what the user selected
    std::vector<Item> _items;       // flattened tree (built on first GetData)
    std::vector<CFolderFetch *> _fetches;   // top-level folder contexts (owned)
    BOOL _expanded = FALSE;
    BOOL _probeBorn = FALSE;        // created inside a shell menu/drag probe window
    DWORD _probeTick = 0;           // and when that happened (see BeingProbed)
    BOOL _prewarmStarted = FALSE;   // background subtree warm-up already kicked off
    BOOL _asyncMode = FALSE;        // IDataObjectAsyncCapability: target opted into async
    BOOL _inOperation = FALSE;      // IDataObjectAsyncCapability: between Start/EndOperation
};
