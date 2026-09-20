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
            if (_fetchStarted && !_downloadReady && !_cancelled && !_cancelRequested && !_batchId.empty())
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
        DWORD got = 0;
        if (!ReadFile(_h, pv, cb, &got, NULL))
        {
            ProbeLog(L"[DL] ReadFile failed cb=%lu err=%lu local='%s'",
                     (unsigned long)cb, (unsigned long)GetLastError(), _local.c_str());
            return STG_E_READFAULT;
        }
        if (!_loggedFirstRead)
        {
            _loggedFirstRead = TRUE;
            ProbeLog(L"[DL] first Read ok cb=%lu got=%lu size=%llu local='%s'",
                     (unsigned long)cb, (unsigned long)got, (unsigned long long)_size, _local.c_str());
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

        BOOL fetched = FALSE;
        for (int attempt = 0; attempt < 2 && !fetched; ++attempt)
        {
            if (attempt)
            {
                ProbeLog(L"[DL] retrying download (attempt %d) remote='%s'", attempt + 1, _remote.c_str());
                Sleep(300);
            }
            std::string reply;
            FtpBridgeFetchState state = RfsFetchToFile(_site.c_str(), _remote.c_str(), _local.c_str(), _batchId.c_str(), reply);
            fetched = (state == FtpBridgeFetchState::Done);
            if (state == FtpBridgeFetchState::Cancelled)
            {
                _cancelled = TRUE;
                ProbeLog(L"[DL] cancelled terminal; not retrying batch='%s' remote='%s'", _batchId.c_str(), _remote.c_str());
                break;
            }
        }
        if (!fetched)
        {
            ProbeLog(L"[DL] Ensure FAILED: download failed site='%s' remote='%s' local='%s' exists=%d cancelled=%d",
                     _site.c_str(), _remote.c_str(), _local.c_str(),
                     (int)PathFileExistsW(_local.c_str()), (int)_cancelled);
            DeleteFileW(_local.c_str());       // existing policy: no partial data-object cache
            _local.clear();
            if (!_cancelled) _done = FALSE;    // ordinary failure may still be retried
            return FALSE;
        }
        _h = CreateFileW(_local.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (_h == INVALID_HANDLE_VALUE)
        {
            ProbeLog(L"[DL] Ensure FAILED: cannot open local '%s' err=%lu",
                     _local.c_str(), (unsigned long)GetLastError());
            DeleteFileW(_local.c_str());
            _local.clear();
            return FALSE;
        }
        {
            LARGE_INTEGER li = {};
            GetFileSizeEx(_h, &li);
            ProbeLog(L"[DL] Ensure ok local='%s' size_on_disk=%lld expected=%llu",
                     _local.c_str(), (long long)li.QuadPart, (unsigned long long)_size);
        }
        _downloadReady = TRUE;
        ProbeLog(L"[DL] Ensure done tid=%lu elapsedMs=%llu local='%s' batch='%s'",
                 GetCurrentThreadId(), GetTickCount64() - tEnsure, _local.c_str(), _batchId.c_str());
        return TRUE;
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
class CRemoteDataObject : public IDataObject
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
        return E_NOINTERFACE;
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
                              : FtpListCachedAll(dir.site.c_str(), full.c_str(), kids);
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
};
