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

// Synchronous `cli get <site> <remote> <local>`; TRUE on success. (Top-level
// single-file copies still use this — one job per file.)
inline BOOL RfsFetchToFile(PCWSTR site, PCWSTR remote, PCWSTR local)
{
    std::wstring cli = RfsCliPath();
    if (cli.empty() || GetFileAttributesW(cli.c_str()) == INVALID_FILE_ATTRIBUTES) return FALSE;
    std::wstring cmd = L"\"" + cli + L"\" get \"" + site + L"\" \"" + remote + L"\" \"" + local + L"\"";
    std::vector<WCHAR> line(cmd.begin(), cmd.end());
    line.push_back(0);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, line.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return FALSE;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ProbeLog(L"[DATAOBJ] fetch '%s' -> '%s' exit=%u", remote, local, code);
    return code == 0;
}

// ---------------------------------------------------------------------------
// Lazy stream: downloads once, then serves the local temp file.
// (Top-level single-file path.)
// ---------------------------------------------------------------------------
class CRemoteStream : public IStream
{
public:
    CRemoteStream(PCWSTR site, PCWSTR remote, ULONGLONG size)
        : _ref(1), _site(site ? site : L""), _remote(remote ? remote : L""),
          _size(size), _pos(0), _h(INVALID_HANDLE_VALUE), _done(FALSE)
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
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP Read(void *pv, ULONG cb, ULONG *pcbRead) override
    {
        if (pcbRead) *pcbRead = 0;
        if (!pv) return STG_E_INVALIDPOINTER;
        if (!Ensure()) return STG_E_READFAULT;
        DWORD got = 0;
        if (!ReadFile(_h, pv, cb, &got, NULL)) return STG_E_READFAULT;
        if (pcbRead) *pcbRead = got;
        _pos += got;
        return got == 0 ? S_FALSE : S_OK;
    }
    STDMETHODIMP Write(const void *, ULONG, ULONG *) override { return STG_E_ACCESSDENIED; }

    STDMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *newPos) override
    {
        if (!Ensure()) return STG_E_READFAULT;
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
        if (!Ensure()) return STG_E_READFAULT;
        std::vector<BYTE> buf(64 * 1024);
        ULONGLONG totalRead = 0, totalWritten = 0;
        while (totalRead < cb.QuadPart)
        {
            ULONGLONG want64 = (ULONGLONG)buf.size();
            if (want64 > cb.QuadPart - totalRead) want64 = cb.QuadPart - totalRead;
            ULONG want = (ULONG)want64;
            ULONG got = 0;
            if (FAILED(Read(buf.data(), want, &got)) || got == 0) break;
            ULONG put = 0;
            if (FAILED(dst->Write(buf.data(), got, &put))) break;
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
        _done = TRUE;
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
        if (!RfsFetchToFile(_site.c_str(), _remote.c_str(), _local.c_str()))
        {
            _local.clear();
            return FALSE;
        }
        _h = CreateFileW(_local.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (_h == INVALID_HANDLE_VALUE) _local.clear();
        return _h != INVALID_HANDLE_VALUE;
    }

    LONG _ref;
    std::wstring _site, _remote, _local;
    ULONGLONG _size, _pos;
    HANDLE _h;
    BOOL _done;
};

// ---------------------------------------------------------------------------
// Folder fetch: ONE 'cli getr' process downloads the whole tree into a temp
// root, writing each file to <name>.rfs-part and renaming on completion (so a
// waiter never sees a half-written file). Streams served from the local tree.
// Refcounted: the data object + every live stream hold a ref; at 0 the process
// is killed (if still running) and the temp tree is deleted.
// ---------------------------------------------------------------------------
class CFolderFetch
{
public:
    CFolderFetch(PCWSTR site, PCWSTR remoteDir)
        : _ref(1), _proc(NULL), _started(FALSE), _seq(0)
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
        InitializeCriticalSection(&_cs);
    }

    ~CFolderFetch()
    {
        if (_proc)
        {
            DWORD code = STILL_ACTIVE;
            if (GetExitCodeProcess(_proc, &code) && code == STILL_ACTIVE)
                TerminateProcess(_proc, 1);
            CloseHandle(_proc);
        }
        DeleteTree(_localRoot);
        DeleteCriticalSection(&_cs);
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

        std::wstring cli = RfsCliPath();
        if (cli.empty() || _localRoot.empty())
        {
            ProbeLog(L"[DATAOBJ] getr cannot start (cli/root empty)");
            return;
        }
        std::wstring cmd = L"\"" + cli + L"\" getr \"" + _site + L"\" \"" + _remoteDir + L"\" \"" + _localRoot + L"\"";
        std::vector<WCHAR> line(cmd.begin(), cmd.end());
        line.push_back(0);
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        if (CreateProcessW(NULL, line.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        {
            _proc = pi.hProcess;
            CloseHandle(pi.hThread);
            ProbeLog(L"[DATAOBJ] getr started site='%s' dir='%s' root='%s' pid=%u",
                     _site.c_str(), _remoteDir.c_str(), _localRoot.c_str(), pi.dwProcessId);
        }
        else
        {
            ProbeLog(L"[DATAOBJ] getr CreateProcess FAILED err=%u", GetLastError());
        }
    }

    std::wstring LocalPath(const std::wstring &rel) const
    {
        std::wstring p = _localRoot;
        p += L"\\";
        p += rel;       // rel already uses backslash separators
        return p;
    }

    // Wait until the file appears (the 'getr' process renames it into place
    // when its download completes). Returns FALSE if the process ended without
    // producing this file.
    BOOL WaitForFile(const std::wstring &rel)
    {
        std::wstring p = LocalPath(rel);
        for (;;)
        {
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
                return TRUE;
            if (_proc)
            {
                DWORD code = STILL_ACTIVE;
                if (GetExitCodeProcess(_proc, &code) && code != STILL_ACTIVE)
                {
                    // Process ended — give the final rename a brief moment to
                    // land, then a last check.
                    for (int i = 0; i < 5; i++)
                    {
                        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return TRUE;
                        Sleep(80);
                    }
                    return FALSE;
                }
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
    HANDLE _proc;
    BOOL _started;
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

    static const size_t kMaxItems = 5000;

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

    void ExpandIfNeeded()
    {
        if (_expanded) return;
        _expanded = TRUE;
        _items.clear();
        for (size_t i = 0; i < _tops.size(); i++)
        {
            CFolderFetch *fetch = nullptr;
            if (_tops[i].isFolder)
            {
                std::wstring full = _tops[i].folder;
                if (!full.empty() && full[full.size() - 1] != L'/') full += L'/';
                full += _tops[i].name;
                fetch = new (std::nothrow) CFolderFetch(_tops[i].site.c_str(), full.c_str());
                if (fetch) _fetches.push_back(fetch);
            }
            ExpandInto(_tops[i], fetch);
        }
        ProbeLog(L"[DATAOBJ] expanded tops=%u items=%u%s", (UINT)_tops.size(), (UINT)_items.size(),
                 _items.size() >= kMaxItems ? L" (truncated)" : L"");
    }

    void ExpandInto(const Item &dir, CFolderFetch *fetch)
    {
        if (_items.size() >= kMaxItems) return;
        Item it = dir;
        it.fetch = fetch;
        _items.push_back(it);
        if (!dir.isFolder) return;

        std::wstring full = dir.folder;
        if (!full.empty() && full[full.size() - 1] != L'/') full += L'/';
        full += dir.name;

        std::vector<FTPENTRY> kids;
        if (!FtpListCachedAll(dir.site.c_str(), full.c_str(), kids))
        {
            ProbeLog(L"[DATAOBJ] list failed '%s'", full.c_str());
            return;
        }
        for (size_t k = 0; k < kids.size(); k++)
        {
            if (_items.size() >= kMaxItems) return;
            Item it2;
            it2.site = dir.site;
            it2.folder = full;
            it2.name = kids[k].szName;
            it2.relPath = dir.relPath + L"\\" + kids[k].szName;
            it2.size = kids[k].dwSize;
            it2.mtime = kids[k].dwMtime;
            it2.isFolder = kids[k].fIsFolder;
            it2.fetch = fetch;       // inherit the folder's fetch context
            ExpandInto(it2, fetch);
        }
    }

    STDMETHODIMP GetData(FORMATETC *fmt, STGMEDIUM *medium) override
    {
        if (!fmt || !medium) return E_INVALIDARG;
        ExpandIfNeeded();
        ZeroMemory(medium, sizeof(*medium));
        CLIPFORMAT cfDesc = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
        CLIPFORMAT cfContents = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILECONTENTS);

        if (fmt->cfFormat == cfDesc)
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
                    ProbeLog(L"[DATAOBJ] fetch wait failed '%s'", it.relPath.c_str());
                    return STG_E_READFAULT;
                }
                std::wstring local = it.fetch->LocalPath(it.relPath);
                CLocalStream *stream = new (std::nothrow) CLocalStream(local.c_str(), it.size, it.fetch);
                if (!stream) return E_OUTOFMEMORY;
                medium->tymed = TYMED_ISTREAM;
                medium->pstm = stream;
                medium->pUnkForRelease = NULL;
                ProbeLog(L"[DATAOBJ] GetData contents idx=%d '%s' (from folder fetch)", (int)fmt->lindex, it.name.c_str());
                return S_OK;
            }

            // Top-level single file: one 'get' per file.
            std::wstring remote = it.folder;
            if (!remote.empty() && remote[remote.size() - 1] != L'/') remote += L'/';
            remote += it.name;
            CRemoteStream *stream = new (std::nothrow) CRemoteStream(it.site.c_str(), remote.c_str(), it.size);
            if (!stream) return E_OUTOFMEMORY;
            medium->tymed = TYMED_ISTREAM;
            medium->pstm = stream;
            medium->pUnkForRelease = NULL;
            ProbeLog(L"[DATAOBJ] GetData contents idx=%d '%s'", (int)fmt->lindex, it.name.c_str());
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
    CRemoteDataObject() : _ref(1) {}
    ~CRemoteDataObject()
    {
        for (size_t i = 0; i < _fetches.size(); i++) _fetches[i]->Release();
    }

    LONG _ref;
    std::vector<Item> _tops;        // what the user selected
    std::vector<Item> _items;       // flattened tree (built on first GetData)
    std::vector<CFolderFetch *> _fetches;   // top-level folder contexts (owned)
    BOOL _expanded = FALSE;
};
