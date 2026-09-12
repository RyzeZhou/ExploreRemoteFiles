#pragma once
// ---------------------------------------------------------------------------
// RemoteDataObject.h — a virtual-file IDataObject for remote items.
//
// WHY: Explorer implements Copy / Cut / Move-to / Copy-to / Paste-shortcut and
// "drag out to a local folder" through the shell's data-object + file-operation
// engine. It never sends a canonical verb for those, so a namespace extension
// must hand it a data object that can actually produce file CONTENT.
//
// DESIGN: file descriptors (name / size / mtime / attributes) are produced
// immediately from the enumeration we already have, so Explorer can render its
// own progress; the bytes are fetched LAZILY, the first time the shell reads
// the stream (IStream::Read), into a temp file via the CLI's `get`.
// ---------------------------------------------------------------------------

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <string>
#include <vector>
#include "ProbeLog.h"

// Local path of the transfer CLI (per-user install location).
inline std::wstring RfsCliPath()
{
    WCHAR dir[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, dir))) return L"";
    std::wstring p = dir;
    p += L"\\ExplorerRemoteFs\\cli\\ExplorerRemoteFs.Cli.exe";
    return p;
}

// Synchronous `cli get <site> <remote> <local>`; TRUE on success.
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
// ---------------------------------------------------------------------------
class CRemoteStream : public IStream
{
public:
    CRemoteStream(PCWSTR site, PCWSTR remote, ULONGLONG size)
        : _ref(1), _site(site ? site : L""), _remote(remote ? remote : L""),
          _size(size), _pos(0), _h(INVALID_HANDLE_VALUE), _done(FALSE)
    {
    }

    // IUnknown
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

    // ISequentialStream
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

    // IStream
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
// Data object: descriptors up front, contents lazily.
// ---------------------------------------------------------------------------
class CRemoteDataObject : public IDataObject
{
public:
    struct Item
    {
        std::wstring site, folder, name;
        ULONGLONG size;
        DWORD mtime;
        BOOL isFolder;
    };

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
        it.size = size;
        it.mtime = mtime;
        it.isFolder = isFolder;
        _items.push_back(it);
    }

    // IUnknown
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

    // ---- IDataObject ----------------------------------------------------
    STDMETHODIMP GetData(FORMATETC *fmt, STGMEDIUM *medium) override
    {
        if (!fmt || !medium) return E_INVALIDARG;
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
                StringCchCopyW(fd.cFileName, ARRAYSIZE(fd.cFileName), _items[i].name.c_str());
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
            if (it.isFolder) return DV_E_LINDEX;      // folders have no byte stream
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
    ~CRemoteDataObject() {}

    LONG _ref;
    std::vector<Item> _items;
};
