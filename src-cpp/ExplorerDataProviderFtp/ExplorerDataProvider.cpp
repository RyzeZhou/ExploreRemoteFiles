/**************************************************************************
    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
   ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
   THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
   PARTICULAR PURPOSE.

   (c) Microsoft Corporation. All Rights Reserved.
**************************************************************************/

#include <windows.h>
#include <shlobj.h>
#include <propkey.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <shellapi.h>
#include <new>  // std::nothrow
#include <string>
#include <vector>
#include <time.h>

#include "resource.h"
#include "Utils.h"
#include "ProbeLog.h"
#include "Category.h"
#include "Guid.h"
#include "fvcommands.h"

const int g_nMaxLevel = 5;

HRESULT CFolderViewCB_CreateInstance(REFIID riid, void **ppv);


#define MYOBJID 0x1234

// FVITEMID is allocated with a variable size, szName is the beginning
// of a NULL-terminated string buffer.
#pragma pack(1)
typedef struct tagObject
{
    USHORT  cb;
    WORD    MyObjID;
    BYTE    nLevel;
    BYTE    nSize;
    BYTE    nSides;
    BYTE    cchName;
    BOOL    fIsFolder;
    WCHAR   szName[1];
} FVITEMID;
#pragma pack()

typedef UNALIGNED FVITEMID *PFVITEMID;
typedef const UNALIGNED FVITEMID *PCFVITEMID;

class CFolderViewImplFolder : public IShellFolder2,
                              public IPersistFolder2
{
public:
    CFolderViewImplFolder(UINT nLevel, PCWSTR pszRemotePath);

    // IUnknown methods
    IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv);
    IFACEMETHODIMP_(ULONG) AddRef();
    IFACEMETHODIMP_(ULONG) Release();

    // IShellFolder
    IFACEMETHODIMP ParseDisplayName(HWND hwnd, IBindCtx *pbc, PWSTR pszName,
                                    ULONG *pchEaten, PIDLIST_RELATIVE *ppidl, ULONG *pdwAttributes);
    IFACEMETHODIMP EnumObjects(HWND hwnd, DWORD grfFlags, IEnumIDList **ppenumIDList);
    IFACEMETHODIMP BindToObject(PCUIDLIST_RELATIVE pidl, IBindCtx *pbc, REFIID riid, void **ppv);
    IFACEMETHODIMP BindToStorage(PCUIDLIST_RELATIVE pidl, IBindCtx *pbc, REFIID riid, void **ppv);
    IFACEMETHODIMP CompareIDs(LPARAM lParam, PCUIDLIST_RELATIVE pidl1, PCUIDLIST_RELATIVE pidl2);
    IFACEMETHODIMP CreateViewObject(HWND hwnd, REFIID riid, void **ppv);
    IFACEMETHODIMP GetAttributesOf(UINT cidl, PCUITEMID_CHILD_ARRAY apidl, ULONG *rgfInOut);
    IFACEMETHODIMP GetUIObjectOf(HWND hwnd, UINT cidl, PCUITEMID_CHILD_ARRAY apidl,
                                 REFIID riid, UINT* prgfInOut, void **ppv);
    IFACEMETHODIMP GetDisplayNameOf(PCUITEMID_CHILD pidl, SHGDNF shgdnFlags, STRRET *pName);
    IFACEMETHODIMP SetNameOf(HWND hwnd, PCUITEMID_CHILD pidl, PCWSTR pszName, DWORD uFlags, PITEMID_CHILD * ppidlOut);

    // IShellFolder2
    IFACEMETHODIMP GetDefaultSearchGUID(GUID *pGuid);
    IFACEMETHODIMP EnumSearches(IEnumExtraSearch **ppenum);
    IFACEMETHODIMP GetDefaultColumn(DWORD dwRes, ULONG *pSort, ULONG *pDisplay);
    IFACEMETHODIMP GetDefaultColumnState(UINT iColumn, SHCOLSTATEF *pbState);
    IFACEMETHODIMP GetDetailsEx(PCUITEMID_CHILD pidl, const PROPERTYKEY *pkey, VARIANT *pv);
    IFACEMETHODIMP GetDetailsOf(PCUITEMID_CHILD pidl, UINT iColumn, SHELLDETAILS *pDetails);
    IFACEMETHODIMP MapColumnToSCID(UINT iColumn, PROPERTYKEY *pkey);

    // IPersist
    IFACEMETHODIMP GetClassID(CLSID *pClassID);

    // IPersistFolder
    IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE pidl);

    // IPersistFolder2
    IFACEMETHODIMP GetCurFolder(PIDLIST_ABSOLUTE *ppidl);

    // IDList constructor public for the enumerator object
    HRESULT CreateChildID(PCWSTR pszName, int nLevel, int nSize, int nSides, BOOL fIsFolder, PITEMID_CHILD *ppidl);

private:
    ~CFolderViewImplFolder();

    HRESULT _GetName(PCUIDLIST_RELATIVE pidl, PWSTR pszName, int cchMax);
    HRESULT _GetName(PCUIDLIST_RELATIVE pidl, PWSTR *pszName);
    HRESULT _GetSides(PCUIDLIST_RELATIVE pidl, int* pSides);
    HRESULT _GetLevel(PCUIDLIST_RELATIVE pidl, int* pLevel);
    HRESULT _GetSize(PCUIDLIST_RELATIVE pidl, int* pSize);
    HRESULT _GetFolderness(PCUIDLIST_RELATIVE pidl, BOOL* pfIsFolder);
    HRESULT _ValidatePidl(PCUIDLIST_RELATIVE pidl);
    PCFVITEMID _IsValid(PCUIDLIST_RELATIVE pidl);

    HRESULT _GetColumnDisplayName(PCUITEMID_CHILD pidl, const PROPERTYKEY* pkey, VARIANT* pv, WCHAR* pszRet, UINT cch);

    long                m_cRef;
    int                 m_nLevel;
    PIDLIST_ABSOLUTE    m_pidl;             // where this folder is in the name space
    PWSTR               m_rgNames[MAX_OBJS];
    WCHAR               m_szModuleName[MAX_PATH];
    WCHAR               m_szRemotePath[512];
};

typedef struct
{
    int     nLevel;
    DWORD   dwMode;       // unix permission bits
    DWORD   dwSize;       // bytes
    DWORD   dwMtime;      // unix epoch seconds
    BOOL    fIsFolder;
    BOOL    fIsSymlink;
    WCHAR   szOwner[40];
    WCHAR   szGroup[40];
    WCHAR   szName[MAX_PATH];
} ITEMDATA;

static DWORD ModeFromString(const WCHAR *pszMode)
{
    DWORD mode = 0;
    if (!pszMode) return 0;
    for (int i = 0; i < 9; i++)
    {
        if (pszMode[i + 1] != L'-')
        {
            mode |= (0400 >> i);
        }
    }
    return mode;
}
static BOOL RunFtpList(PCWSTR path, ITEMDATA *out, int maxItems)
{
    WCHAR cmd[1200];
    StringCchPrintf(cmd, ARRAYSIZE(cmd),
        L"\"D:\\tools\\explorer-remote-fs\\dist\\cli\\ExplorerRemoteFs.Cli.exe\" pipe \"local-ftp\" \"%s\"",
        (path && path[0]) ? path : L"/");
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return FALSE;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    BOOL spawned = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(wr);
    if (!spawned) { CloseHandle(rd); return FALSE; }
    std::string text; char buf[4096]; DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, NULL) && got) text.append(buf, got);
    CloseHandle(rd); WaitForSingleObject(pi.hProcess, 8000); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    int count = 0; size_t pos = 0;
    while (count < maxItems && pos < text.size())
    {
        size_t end = text.find('\n', pos); if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos); pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("ITEM\t", 0) != 0) continue;
        WCHAR wide[2048];
        if (!MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, wide, ARRAYSIZE(wide))) continue;
        WCHAR *fields[10] = {}; int nf = 0; WCHAR *ctx = NULL;
        WCHAR *tok = wcstok_s(wide, L"\t", &ctx);
        while (tok && nf < 10) { fields[nf++] = tok; tok = wcstok_s(NULL, L"\t", &ctx); }
        if (nf < 10) continue;
        ITEMDATA &item = out[count];
        item.nLevel = 0;
        item.dwMode  = ModeFromString(fields[1]);
        item.dwMtime = (DWORD)_wtoi64(fields[2]);
        item.dwSize  = (DWORD)_wtoi64(fields[3]);
        StringCchCopy(item.szOwner, ARRAYSIZE(item.szOwner), fields[4]);
        StringCchCopy(item.szGroup, ARRAYSIZE(item.szGroup), fields[5]);
        item.fIsFolder  = _wtoi(fields[6]) != 0;
        item.fIsSymlink = _wtoi(fields[7]) != 0;
        StringCchCopy(item.szName, ARRAYSIZE(item.szName), fields[9]);
        count++;
    }
    ProbeLog(L"[FTP-SAMPLE] list path='%s' count=%d", path, count);
    return count >= 0 ? count : 0;
}
static int RunFtpOperation(PCWSTR verb, PCWSTR path1, PCWSTR path2)
{
    WCHAR cmd[1600];
    HRESULT hr = path2 && path2[0]
        ? StringCchPrintf(cmd, ARRAYSIZE(cmd), L"\"D:\\tools\\explorer-remote-fs\\dist\\cli\\ExplorerRemoteFs.Cli.exe\" %s \"local-ftp\" \"%s\" \"%s\"", verb, path1, path2)
        : StringCchPrintf(cmd, ARRAYSIZE(cmd), L"\"D:\\tools\\explorer-remote-fs\\dist\\cli\\ExplorerRemoteFs.Cli.exe\" %s \"local-ftp\" \"%s\"", verb, path1);
    if (FAILED(hr)) return -1;
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (!ok) return -1;
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    ProbeLog(L"[FTP-SAMPLE] op=%s path1='%s' path2='%s' exit=%u", verb, path1, path2 ? path2 : L"", code);
    return code == 0 ? 0 : -1;
}
static BOOL GetItemMeta(PCWSTR path, PCWSTR name, ITEMDATA *out)
{
    if (!name || !name[0]) return FALSE;
    ITEMDATA items[MAX_OBJS] = {};
    RunFtpList(path ? path : L"/", items, ARRAYSIZE(items));
    for (int i = 0; i < ARRAYSIZE(items) && items[i].szName[0]; i++)
    {
        if (0 == StrCmpI(items[i].szName, name))
        {
            *out = items[i];
            return TRUE;
        }
    }
    return FALSE;
}
static void FormatMode(DWORD mode, BOOL folder, BOOL symlink, PWSTR out, UINT cch)
{
    WCHAR type = symlink ? L'l' : (folder ? L'd' : L'-');
    StringCchPrintf(out, cch, L"%c%c%c%c%c%c%c%c%c%c", type,
        (mode & 0400) ? L'r' : L'-', (mode & 0200) ? L'w' : L'-', (mode & 0100) ? L'x' : L'-',
        (mode & 0040) ? L'r' : L'-', (mode & 0020) ? L'w' : L'-', (mode & 0010) ? L'x' : L'-',
        (mode & 0004) ? L'r' : L'-', (mode & 0002) ? L'w' : L'-', (mode & 0001) ? L'x' : L'-');
}
static void FormatSize(DWORD size, BOOL folder, PWSTR out, UINT cch)
{
    if (folder) { StringCchCopy(out, cch, L"-"); return; }
    if (size < 1024) StringCchPrintf(out, cch, L"%u B", size);
    else if (size < 1024*1024) StringCchPrintf(out, cch, L"%.1f KB", size / 1024.0);
    else if (size < 1024*1024*1024) StringCchPrintf(out, cch, L"%.1f MB", size / (1024.0*1024.0));
    else StringCchPrintf(out, cch, L"%.2f GB", size / (1024.0*1024.0*1024.0));
}
static void FormatMtime(DWORD mtime, PWSTR out, UINT cch)
{
    __time64_t t = (__time64_t)mtime;
    struct tm tmLocal;
    if (_localtime64_s(&tmLocal, &t) == 0)
        StringCchPrintf(out, cch, L"%04d-%02d-%02d %02d:%02d",
            tmLocal.tm_year + 1900, tmLocal.tm_mon + 1, tmLocal.tm_mday,
            tmLocal.tm_hour, tmLocal.tm_min);
    else StringCchCopy(out, cch, L"-");
}

class CFolderViewImplEnumIDList : public IEnumIDList
{
public:
    CFolderViewImplEnumIDList(DWORD grfFlags, int nCurrent, PCWSTR pszPath, CFolderViewImplFolder *pFolderViewImplShellFolder);

    // IUnknown methods
    IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv);
    IFACEMETHODIMP_(ULONG) AddRef();
    IFACEMETHODIMP_(ULONG) Release();

    // IEnumIDList
    IFACEMETHODIMP Next(ULONG celt, PITEMID_CHILD *rgelt, ULONG *pceltFetched);
    IFACEMETHODIMP Skip(DWORD celt);
    IFACEMETHODIMP Reset();
    IFACEMETHODIMP Clone(IEnumIDList **ppenum);

    HRESULT Initialize();

private:
    ~CFolderViewImplEnumIDList();

    long m_cRef;
    DWORD m_grfFlags;
    int m_nItem;
    int m_nLevel;
    ITEMDATA m_aData[MAX_OBJS];
    WCHAR m_szPath[512];

    CFolderViewImplFolder *m_pFolder;
};

HRESULT CFolderViewImplFolder_CreateInstance(REFIID riid, void **ppv)
{
    *ppv = NULL;
    CFolderViewImplFolder* pFolderViewImplShellFolder = new (std::nothrow) CFolderViewImplFolder(0, L"");
    HRESULT hr = pFolderViewImplShellFolder ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(hr))
    {
        hr = pFolderViewImplShellFolder->QueryInterface(riid, ppv);
        pFolderViewImplShellFolder->Release();
    }
    return hr;
}

CFolderViewImplFolder::CFolderViewImplFolder(UINT nLevel, PCWSTR pszRemotePath) : m_cRef(1), m_nLevel(nLevel), m_pidl(NULL)
{
    DllAddRef();
    StringCchCopy(m_szRemotePath, ARRAYSIZE(m_szRemotePath), pszRemotePath ? pszRemotePath : L"");
    ZeroMemory(m_rgNames, sizeof(m_rgNames));
}

CFolderViewImplFolder::~CFolderViewImplFolder()
{
    CoTaskMemFree(m_pidl);
    for (int i = 0; i < ARRAYSIZE(m_rgNames); i++)
    {
        CoTaskMemFree(m_rgNames[i]);
    }
    DllRelease();
}

HRESULT CFolderViewImplFolder::QueryInterface(REFIID riid, void **ppv)
{
    static const QITAB qit[] =
    {
        QITABENT(CFolderViewImplFolder, IShellFolder),
        QITABENT(CFolderViewImplFolder, IShellFolder2),
        QITABENT(CFolderViewImplFolder, IPersist),
        QITABENT(CFolderViewImplFolder, IPersistFolder),
        QITABENT(CFolderViewImplFolder, IPersistFolder2),
        { 0 },
    };
    return QISearch(this, qit, riid, ppv);
}

ULONG CFolderViewImplFolder::AddRef()
{
    return InterlockedIncrement(&m_cRef);
}

ULONG CFolderViewImplFolder::Release()
{
    long cRef = InterlockedDecrement(&m_cRef);
    if (0 == cRef)
    {
        delete this;
    }
    return cRef;
}

//  Translates a display name into an item identifier list.
HRESULT CFolderViewImplFolder::ParseDisplayName(HWND hwnd, IBindCtx *pbc, PWSTR pszName,
                                                ULONG *pchEaten, PIDLIST_RELATIVE *ppidl, ULONG *pdwAttributes)
{
    if (!pszName || !ppidl) return E_INVALIDARG;
    *ppidl = NULL;
    WCHAR component[MAX_PATH] = {};
    PWSTR next = PathFindNextComponent(pszName);
    HRESULT hr = (next && *next)
        ? StringCchCopyN(component, ARRAYSIZE(component), pszName, lstrlen(pszName) - lstrlen(next))
        : StringCchCopy(component, ARRAYSIZE(component), pszName);
    if (FAILED(hr)) return hr;
    PathRemoveBackslash(component);
    ITEMDATA items[MAX_OBJS] = {};
    RunFtpList(m_szRemotePath, items, ARRAYSIZE(items));
    for (int i = 0; i < ARRAYSIZE(items) && items[i].szName[0]; i++)
    {
        if (0 != StrCmpI(items[i].szName, component)) continue;
        PIDLIST_RELATIVE current = NULL;
        hr = CreateChildID(component, m_nLevel + 1, 1, 3, items[i].fIsFolder, &current);
        if (FAILED(hr)) return hr;
        if (next && *next)
        {
            IShellFolder *child = NULL;
            hr = BindToObject(current, pbc, IID_PPV_ARGS(&child));
            if (SUCCEEDED(hr))
            {
                PIDLIST_RELATIVE tail = NULL;
                hr = child->ParseDisplayName(hwnd, pbc, next, pchEaten, &tail, pdwAttributes);
                if (SUCCEEDED(hr)) { *ppidl = ILCombine(current, tail); ILFree(tail); }
                child->Release();
            }
            ILFree(current);
        }
        else *ppidl = current;
        return hr;
    }
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

//  Allows a client to determine the contents of a folder by
//  creating an item identifier enumeration object and returning
//  its IEnumIDList interface. The methods supported by that
//  interface can then be used to enumerate the folder's contents.
HRESULT CFolderViewImplFolder::EnumObjects(HWND /* hwnd */, DWORD grfFlags, IEnumIDList **ppenumIDList)
{
    ProbeLog(L"[SAMPLE] EnumObjects level=%d", m_nLevel);
    HRESULT hr;
    if (m_nLevel >= g_nMaxLevel)
    {
        *ppenumIDList = NULL;
        hr = S_FALSE; // S_FALSE is allowed with NULL out param to indicate no contents.
    }
    else
    {
        CFolderViewImplEnumIDList *penum = new (std::nothrow) CFolderViewImplEnumIDList(grfFlags, m_nLevel + 1, m_szRemotePath, this);
        hr = penum ? S_OK : E_OUTOFMEMORY;
        if (SUCCEEDED(hr))
        {
            hr = penum->Initialize();
            if (SUCCEEDED(hr))
            {
                hr = penum->QueryInterface(IID_PPV_ARGS(ppenumIDList));
            }
            penum->Release();
        }
    }

    return hr;
}


//  Factory for handlers for the specified item.
HRESULT CFolderViewImplFolder::BindToObject(PCUIDLIST_RELATIVE pidl,
                                            IBindCtx *pbc, REFIID riid, void **ppv)
{
    *ppv = NULL;
    ProbeLog(L"[FTP-SAMPLE] BindToObject level=%d path='%s'", m_nLevel, m_szRemotePath);
    HRESULT hr = _ValidatePidl(pidl);
    if (SUCCEEDED(hr))
    {
        WCHAR name[MAX_PATH];
        hr = _GetName(pidl, name, ARRAYSIZE(name));
        if (SUCCEEDED(hr))
        {
            WCHAR childPath[512];
            if (!m_szRemotePath[0] || (m_szRemotePath[0] == L'/' && !m_szRemotePath[1]))
                StringCchPrintf(childPath, ARRAYSIZE(childPath), L"/%s", name);
            else
                StringCchPrintf(childPath, ARRAYSIZE(childPath), L"%s/%s", m_szRemotePath, name);
            CFolderViewImplFolder *child = new (std::nothrow) CFolderViewImplFolder(m_nLevel + 1, childPath);
            hr = child ? S_OK : E_OUTOFMEMORY;
            if (SUCCEEDED(hr))
            {
                PITEMID_CHILD first = ILCloneFirst(pidl);
                hr = first ? S_OK : E_OUTOFMEMORY;
                if (SUCCEEDED(hr))
                {
                    PIDLIST_ABSOLUTE full = ILCombine(m_pidl, first);
                    hr = full ? S_OK : E_OUTOFMEMORY;
                    if (SUCCEEDED(hr))
                    {
                        hr = child->Initialize(full);
                        if (SUCCEEDED(hr))
                        {
                            PCUIDLIST_RELATIVE next = ILNext(pidl);
                            hr = ILIsEmpty(next) ? child->QueryInterface(riid, ppv) : child->BindToObject(next, pbc, riid, ppv);
                        }
                        CoTaskMemFree(full);
                    }
                    ILFree(first);
                }
                child->Release();
            }
        }
    }
    return hr;
}

HRESULT CFolderViewImplFolder::BindToStorage(PCUIDLIST_RELATIVE pidl,
                                             IBindCtx *pbc, REFIID riid, void **ppv)
{
    return BindToObject(pidl, pbc, riid, ppv);
}


//  Helper function to help compare relative IDs.
HRESULT _ILCompareRelIDs(IShellFolder *psfParent, PCUIDLIST_RELATIVE pidl1, PCUIDLIST_RELATIVE pidl2,
                         LPARAM lParam)
{
    HRESULT hr;
    PCUIDLIST_RELATIVE pidlRel1 = ILNext(pidl1);
    PCUIDLIST_RELATIVE pidlRel2 = ILNext(pidl2);

    if (ILIsEmpty(pidlRel1))
    {
        if (ILIsEmpty(pidlRel2))
        {
            hr = ResultFromShort(0);  // Both empty
        }
        else
        {
            hr = ResultFromShort(-1);   // 1 is empty, 2 is not.
        }
    }
    else
    {
        if (ILIsEmpty(pidlRel2))
        {
            hr = ResultFromShort(1);  // 2 is empty, 1 is not
        }
        else
        {
            // pidlRel1 and pidlRel2 point to something, so:
            //  (1) Bind to the next level of the IShellFolder
            //  (2) Call its CompareIDs to let it compare the rest of IDs.
            PIDLIST_RELATIVE pidlNext = ILCloneFirst(pidl1);    // pidl2 would work as well
            hr = pidlNext ? S_OK : E_OUTOFMEMORY;
            if (pidlNext)
            {
                IShellFolder *psfNext;
                hr = psfParent->BindToObject(pidlNext, NULL, IID_PPV_ARGS(&psfNext));
                if (SUCCEEDED(hr))
                {
                    // We do not want to pass the lParam is IShellFolder2 isn't supported.
                    // Although it isn't important for this example it shoud be considered
                    // if you are implementing this for other situations.
                    IShellFolder2 *psf2;
                    if (SUCCEEDED(psfNext->QueryInterface(&psf2)))
                    {
                        psf2->Release();  // We can use the lParam
                    }
                    else
                    {
                        lParam = 0;       // We can't use the lParam
                    }

                    // Also, the column mask will not be relevant and should never be passed.
                    hr = psfNext->CompareIDs((lParam & ~SHCIDS_COLUMNMASK), pidlRel1, pidlRel2);
                    psfNext->Release();
                }
                CoTaskMemFree(pidlNext);
            }
        }
    }
    return hr;
}

//  Called to determine the equivalence and/or sort order of two idlists.
HRESULT CFolderViewImplFolder::CompareIDs(LPARAM lParam, PCUIDLIST_RELATIVE pidl1, PCUIDLIST_RELATIVE pidl2)
{
    HRESULT hr;
    if (lParam & (SHCIDS_CANONICALONLY | SHCIDS_ALLFIELDS))
    {
        // First do a "canonical" comparison, meaning that we compare with the intent to determine item
        // identity as quickly as possible.  The sort order is arbitrary but it must be consistent.
        PWSTR psz1;
        hr = _GetName(pidl1, &psz1);
        if (SUCCEEDED(hr))
        {
            PWSTR psz2;
            hr = _GetName(pidl2, &psz2);
            if (SUCCEEDED(hr))
            {
                hr = ResultFromShort(StrCmp(psz1, psz2));
                CoTaskMemFree(psz2);
            }
            CoTaskMemFree(psz1);
        }

        // If we've been asked to do an all-fields comparison, test for any other fields that
        // may be different in an item that shares the same identity.  For example if the item
        // represents a file, the identity may be just the filename but the other fields contained
        // in the idlist may be file size and file modified date, and those may change over time.
        // In our example let's say that "level" is the data that could be different on the same item.
        if ((ResultFromShort(0) == hr) && (lParam & SHCIDS_ALLFIELDS))
        {
            int cLevel1 = 0, cLevel2 = 0;
            hr = _GetLevel(pidl1, &cLevel1);
            if (SUCCEEDED(hr))
            {
                hr = _GetLevel(pidl2, &cLevel2);
                if (SUCCEEDED(hr))
                {
                    hr = ResultFromShort(cLevel1 - cLevel2);
                }
            }
        }
    }
    else
    {
        // Compare child ids by column data (lParam & SHCIDS_COLUMNMASK).
        hr = ResultFromShort(0);
        switch (lParam & SHCIDS_COLUMNMASK)
        {
            case 0: // Column one, Name.
            {
                // Load the strings that represent the names
                if (!m_rgNames[0])
                {
                    hr = LoadFolderViewImplDisplayStrings(m_rgNames, ARRAYSIZE(m_rgNames));
                }
                if (SUCCEEDED(hr))
                {
                    PWSTR psz1;
                    hr = _GetName(pidl1, &psz1);
                    if (SUCCEEDED(hr))
                    {
                        PWSTR psz2;
                        hr = _GetName(pidl2, &psz2);
                        if (SUCCEEDED(hr))
                        {
                            // Find their place in the array.
                            // This is a display sort so we want to sort by "one" "two" "three" instead of alphabetically.
                            int nPidlOne = 0, nPidlTwo = 0;
                            for (int i = 0; i < ARRAYSIZE(m_rgNames); i++)
                            {
                                if (0 == StrCmp(psz1, m_rgNames[i]))
                                {
                                    nPidlOne = i;
                                }

                                if (0 == StrCmp(psz2, m_rgNames[i]))
                                {
                                    nPidlTwo = i;
                                }
                            }

                            hr = ResultFromShort(nPidlOne - nPidlTwo);
                            CoTaskMemFree(psz2);
                        }
                        CoTaskMemFree(psz1);
                    }
                }
                break;
            }
            case 1: // Column two, Size.
            {
                int nSize1 = 0, nSize2 = 0;
                hr = _GetSize(pidl1, &nSize1);
                if (SUCCEEDED(hr))
                {
                    hr = _GetSize(pidl2, &nSize2);
                    if (SUCCEEDED(hr))
                    {
                        hr = ResultFromShort(nSize1 - nSize2);
                    }
                }
                break;
            }
            case 2: // Column Three, Sides.
            {
                int nSides1 = 0, nSides2 = 0;
                hr = _GetSides(pidl1, &nSides1);
                if (SUCCEEDED(hr))
                {
                    hr = _GetSides(pidl2, &nSides2);
                    if (SUCCEEDED(hr))
                    {
                        hr = ResultFromShort(nSides1 - nSides2);
                    }
                }
                break;
            }
            case 3: // Column four, Level.
            {
                int cLevel1 = 0, cLevel2 = 0;
                hr = _GetLevel(pidl1, &cLevel1);
                if (SUCCEEDED(hr))
                {
                    hr = _GetLevel(pidl2, &cLevel2);
                    if (SUCCEEDED(hr))
                    {
                        hr = ResultFromShort(cLevel1 - cLevel2);
                    }
                }
                break;
            }
            default:
            {
                hr = ResultFromShort(1);
            }
        }
    }

    if (ResultFromShort(0) == hr)
    {
        // Continue on by binding to the next level.
        hr = _ILCompareRelIDs(this, pidl1, pidl2, lParam);
    }
    return hr;
}

//  Called by the Shell to create the View Object and return it.
HRESULT CFolderViewImplFolder::CreateViewObject(HWND hwnd, REFIID riid, void **ppv)
{
    *ppv = NULL;
    ProbeLog(L"[SAMPLE] CreateViewObject riid=%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X level=%d", riid.Data1, riid.Data2, riid.Data3, riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3], riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7], m_nLevel);

    HRESULT hr = E_NOINTERFACE;
    if (riid == IID_IShellView)
    {
        SFV_CREATE csfv = { sizeof(csfv), 0 };
        hr = QueryInterface(IID_PPV_ARGS(&csfv.pshf));
        if (SUCCEEDED(hr))
        {
            // Add our callback to the SFV_CREATE.  This is optional.  We
            // are adding it so we can enable searching within our
            // namespace.
            hr = CFolderViewCB_CreateInstance(IID_PPV_ARGS(&csfv.psfvcb));
            if (SUCCEEDED(hr))
            {
                hr = SHCreateShellFolderView(&csfv, (IShellView**)ppv);
                csfv.psfvcb->Release();
            }
            csfv.pshf->Release();
        }
    }
    else if (riid == IID_ICategoryProvider)
    {
        CFolderViewImplCategoryProvider* pCatProvider = new (std::nothrow) CFolderViewImplCategoryProvider(this);
        hr = pCatProvider ? S_OK : E_OUTOFMEMORY;
        if (SUCCEEDED(hr))
        {
            hr = pCatProvider->QueryInterface(riid, ppv);
            pCatProvider->Release();
        }
    }
    else if (riid == IID_IContextMenu)
    {
        // This is the background context menu for the folder itself, not the context menu on items within it.
        DEFCONTEXTMENU dcm = { hwnd, NULL, m_pidl, static_cast<IShellFolder2 *>(this), 0, NULL, NULL, 0, NULL };
        hr = SHCreateDefaultContextMenu(&dcm, riid, ppv);
    }
    else if (riid == IID_IExplorerCommandProvider)
    {
        CFolderViewCommandProvider *pProvider = new (std::nothrow) CFolderViewCommandProvider();
        hr = pProvider ? S_OK : E_OUTOFMEMORY;
        if (SUCCEEDED(hr))
        {
            hr = pProvider->QueryInterface(riid, ppv);
            pProvider->Release();
        }
    }
    return hr;
}

//  Retrieves the attributes of one or more file objects or subfolders.
HRESULT CFolderViewImplFolder::GetAttributesOf(UINT cidl, PCUITEMID_CHILD_ARRAY apidl, ULONG *rgfInOut)
{
    // If SFGAO_FILESYSTEM is returned, GetDisplayNameOf(SHGDN_FORPARSING) on that item MUST
    // return a filesystem path.
    HRESULT hr = E_INVALIDARG;
    if (1 == cidl)
    {
        int nLevel = 0;
        hr = _GetLevel(apidl[0], &nLevel);
        if (SUCCEEDED(hr))
        {
            BOOL fIsFolder = FALSE;
            hr = _GetFolderness(apidl[0], &fIsFolder);
            if (SUCCEEDED(hr))
            {
                DWORD dwAttribs = SFGAO_CANRENAME | SFGAO_CANDELETE | SFGAO_HASPROPSHEET;
                if (fIsFolder)
                {
                    dwAttribs |= SFGAO_FOLDER;
                }
                if (nLevel < g_nMaxLevel)
                {
                    dwAttribs |= SFGAO_HASSUBFOLDER;
                }
                *rgfInOut &= dwAttribs;
            }
        }
    }
    return hr;
}

//  Retrieves an OLE interface that can be used to carry out
//  actions on the specified file objects or folders.
HRESULT CFolderViewImplFolder::GetUIObjectOf(HWND hwnd, UINT cidl, PCUITEMID_CHILD_ARRAY apidl,
                                             REFIID riid, UINT * /* prgfInOut */, void **ppv)
{
    *ppv = NULL;
    HRESULT hr;

    if (riid == IID_IContextMenu)
    {
        // The default context menu will call back for IQueryAssociations to determine the
        // file associations with which to populate the menu.
        DEFCONTEXTMENU const dcm = { hwnd, NULL, m_pidl, static_cast<IShellFolder2 *>(this),
                               cidl, apidl, NULL, 0, NULL };
        hr = SHCreateDefaultContextMenu(&dcm, riid, ppv);
    }
    else if (riid == IID_IExtractIconW)
    {
        IDefaultExtractIconInit *pdxi;
        hr = SHCreateDefaultExtractIcon(IID_PPV_ARGS(&pdxi));
        if (SUCCEEDED(hr))
        {
            BOOL fIsFolder = FALSE;
            hr = _GetFolderness(apidl[0], &fIsFolder);
            if (SUCCEEDED(hr))
            {
                // This refers to icon indices in shell32.  You can also supply custom icons or
                // register IExtractImage to support general images.
                hr = pdxi->SetNormalIcon(L"shell32.dll", fIsFolder ? 4 : 1);
            }
            if (SUCCEEDED(hr))
            {
                hr = pdxi->QueryInterface(riid, ppv);
            }
            pdxi->Release();
        }
    }
    else if (riid == IID_IDataObject)
    {
        hr = SHCreateDataObject(m_pidl, cidl, apidl, NULL, riid, ppv);
    }
    else if (riid == IID_IQueryAssociations)
    {
        BOOL fIsFolder = FALSE;
        hr = _GetFolderness(apidl[0], &fIsFolder);
        if (SUCCEEDED(hr))
        {
            // the type of the item can be determined here.  we default to "FolderViewSampleType", which has
            // a context menu registered for it.
            if (fIsFolder)
            {
                ASSOCIATIONELEMENT const rgAssocFolder[] =
                {
                    { ASSOCCLASS_PROGID_STR, NULL, L"RemoteFsMicrosoftCoreType"},
                    { ASSOCCLASS_FOLDER, NULL, NULL},
                };
                hr = AssocCreateForClasses(rgAssocFolder, ARRAYSIZE(rgAssocFolder), riid, ppv);
            }
            else
            {
                ASSOCIATIONELEMENT const rgAssocItem[] =
                {
                    { ASSOCCLASS_PROGID_STR, NULL, L"RemoteFsMicrosoftCoreType"},
                };
                hr = AssocCreateForClasses(rgAssocItem, ARRAYSIZE(rgAssocItem), riid, ppv);
            }
        }
    }
    else
    {
        hr = E_NOINTERFACE;
    }
    return hr;
}

//  Retrieves the display name for the specified file object or subfolder.
HRESULT CFolderViewImplFolder::GetDisplayNameOf(PCUITEMID_CHILD pidl, SHGDNF shgdnFlags, STRRET *pName)
{
    HRESULT hr = S_OK;
    if (shgdnFlags & SHGDN_FORPARSING)
    {
        WCHAR szDisplayName[MAX_PATH];
        if (shgdnFlags & SHGDN_INFOLDER)
        {
            // This form of the display name needs to be handled by ParseDisplayName.
            hr = _GetName(pidl, szDisplayName, ARRAYSIZE(szDisplayName));
        }
        else
        {
            PWSTR pszThisFolder;
            hr = SHGetNameFromIDList(m_pidl, (shgdnFlags & SHGDN_FORADDRESSBAR) ? SIGDN_DESKTOPABSOLUTEEDITING : SIGDN_DESKTOPABSOLUTEPARSING, &pszThisFolder);
            if (SUCCEEDED(hr))
            {
                StringCchCopy(szDisplayName, ARRAYSIZE(szDisplayName), pszThisFolder);
                StringCchCat(szDisplayName, ARRAYSIZE(szDisplayName), L"\\");

                WCHAR szName[MAX_PATH];
                hr = _GetName(pidl, szName, ARRAYSIZE(szName));
                if (SUCCEEDED(hr))
                {
                    StringCchCat(szDisplayName, ARRAYSIZE(szDisplayName), szName);
                }
                CoTaskMemFree(pszThisFolder);
            }
        }
        if (SUCCEEDED(hr))
        {
            hr = StringToStrRet(szDisplayName, pName);
        }
    }
    else
    {
        PWSTR pszName;
        hr = _GetName(pidl, &pszName);
        if (SUCCEEDED(hr))
        {
            hr = StringToStrRet(pszName, pName);
            CoTaskMemFree(pszName);
        }
    }
    return hr;
}

//  Sets the display name of a file object or subfolder, changing
//  the item identifier in the process.
HRESULT CFolderViewImplFolder::SetNameOf(HWND hwnd, PCUITEMID_CHILD pidl,
                                         PCWSTR pszName, DWORD /* uFlags */, PITEMID_CHILD *ppidlOut)
{
    if (ppidlOut) *ppidlOut = NULL;
    if (!pidl || !pszName || !pszName[0]) return E_INVALIDARG;
    WCHAR oldName[MAX_PATH], oldPath[600], newPath[600];
    HRESULT hr = _GetName(pidl, oldName, ARRAYSIZE(oldName));
    if (FAILED(hr)) return hr;
    PCWSTR base = m_szRemotePath[0] ? m_szRemotePath : L"/";
    if (base[0] == L'/' && !base[1])
    {
        StringCchPrintf(oldPath, ARRAYSIZE(oldPath), L"/%s", oldName);
        StringCchPrintf(newPath, ARRAYSIZE(newPath), L"/%s", pszName);
    }
    else
    {
        StringCchPrintf(oldPath, ARRAYSIZE(oldPath), L"%s/%s", base, oldName);
        StringCchPrintf(newPath, ARRAYSIZE(newPath), L"%s/%s", base, pszName);
    }
    if (RunFtpOperation(L"rename", oldPath, newPath) != 0)
    {
        MessageBoxW(hwnd, L"Rename failed.", L"Remote", MB_OK | MB_ICONERROR);
        return E_FAIL;
    }
    BOOL folder = FALSE; int size = 0;
    _GetFolderness(pidl, &folder); _GetSize(pidl, &size);
    if (ppidlOut) hr = CreateChildID(pszName, m_nLevel + 1, size > 0 ? size : 1, 3, folder, ppidlOut);
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST, m_pidl, NULL);
    return SUCCEEDED(hr) ? S_OK : hr;
}

//  IPersist method
HRESULT CFolderViewImplFolder::GetClassID(CLSID *pClassID)
{
    *pClassID = CLSID_FolderViewImpl;
    return S_OK;
}

//  IPersistFolder method
HRESULT CFolderViewImplFolder::Initialize(PCIDLIST_ABSOLUTE pidl)
{
    m_pidl = ILCloneFull(pidl);
    return m_pidl ? S_OK : E_FAIL;
}

//  IShellFolder2 methods
HRESULT CFolderViewImplFolder::EnumSearches(IEnumExtraSearch **ppEnum)
{
    *ppEnum = NULL;
    return E_NOINTERFACE;
}

//  Retrieves the default sorting and display column (indices from GetDetailsOf).
HRESULT CFolderViewImplFolder::GetDefaultColumn(DWORD /* dwRes */,
                                                ULONG *pSort,
                                                ULONG *pDisplay)
{
    *pSort = 0;
    *pDisplay = 0;
    return S_OK;
}

//  Retrieves the default state for a specified column.
HRESULT CFolderViewImplFolder::GetDefaultColumnState(UINT iColumn, SHCOLSTATEF *pcsFlags)
{
    if (iColumn >= 6) return E_INVALIDARG;
    *pcsFlags = SHCOLSTATE_ONBYDEFAULT;
    if (iColumn == 0)          *pcsFlags |= SHCOLSTATE_TYPE_STR;
    else if (iColumn == 1)     *pcsFlags |= SHCOLSTATE_TYPE_STR;   // Permissions
    else if (iColumn == 2)     *pcsFlags |= SHCOLSTATE_TYPE_STR;   // Owner
    else if (iColumn == 3)     *pcsFlags |= SHCOLSTATE_TYPE_STR;   // Group
    else if (iColumn == 4)     *pcsFlags |= SHCOLSTATE_TYPE_INT;   // Size
    else if (iColumn == 5)     *pcsFlags |= SHCOLSTATE_TYPE_DATE;  // Modified
    return S_OK;
}

//  Requests the GUID of the default search object for the folder.
HRESULT CFolderViewImplFolder::GetDefaultSearchGUID(GUID * /* pguid */)
{
    return E_NOTIMPL;
}

//  Helper function for getting the display name for a column.
//  IMPORTANT: If cch is set to 0 the value is returned in the VARIANT.
HRESULT CFolderViewImplFolder::_GetColumnDisplayName(PCUITEMID_CHILD pidl,
                                                     const PROPERTYKEY *pkey,
                                                     VARIANT *pv,
                                                     PWSTR pszRet,
                                                     UINT cch)
{
    BOOL fIsFolder = FALSE;
    HRESULT hr = _GetFolderness(pidl, &fIsFolder);
    if (FAILED(hr)) return hr;
    WCHAR name[MAX_PATH];
    hr = _GetName(pidl, name, ARRAYSIZE(name));
    if (FAILED(hr)) return hr;
    ITEMDATA meta = {};
    GetItemMeta(m_szRemotePath, name, &meta);

    WCHAR szVal[256] = {};
    if (IsEqualPropertyKey(*pkey, PKEY_ItemNameDisplay))
    {
        StringCchCopy(szVal, ARRAYSIZE(szVal), name);
    }
    else if (IsEqualPropertyKey(*pkey, PKEY_Remote_Permissions))
    {
        FormatMode(meta.dwMode, meta.fIsFolder, meta.fIsSymlink, szVal, ARRAYSIZE(szVal));
    }
    else if (IsEqualPropertyKey(*pkey, PKEY_Remote_Owner))
    {
        StringCchCopy(szVal, ARRAYSIZE(szVal), meta.szOwner[0] ? meta.szOwner : L"?");
    }
    else if (IsEqualPropertyKey(*pkey, PKEY_Remote_Group))
    {
        StringCchCopy(szVal, ARRAYSIZE(szVal), meta.szGroup[0] ? meta.szGroup : L"?");
    }
    else if (IsEqualPropertyKey(*pkey, PKEY_Remote_Size))
    {
        FormatSize(meta.dwSize, meta.fIsFolder, szVal, ARRAYSIZE(szVal));
    }
    else if (IsEqualPropertyKey(*pkey, PKEY_Remote_Modified))
    {
        FormatMtime(meta.dwMtime, szVal, ARRAYSIZE(szVal));
    }
    else
    {
        return E_NOTIMPL;
    }

    if (pv != NULL)
    {
        pv->vt = VT_BSTR;
        pv->bstrVal = SysAllocString(szVal);
        hr = pv->bstrVal ? S_OK : E_OUTOFMEMORY;
    }
    else
    {
        hr = StringCchCopy(pszRet, cch, szVal);
    }
    return hr;
}

//  Retrieves detailed information, identified by a
//  property set ID (FMTID) and property ID (PID),
//  on an item in a Shell folder.
HRESULT CFolderViewImplFolder::GetDetailsEx(PCUITEMID_CHILD pidl,
                                            const PROPERTYKEY *pkey,
                                            VARIANT *pv)
{
    BOOL pfIsFolder = FALSE;
    HRESULT hr = _GetFolderness(pidl, &pfIsFolder);
    if (SUCCEEDED(hr))
    {
        if (!pfIsFolder && IsEqualPropertyKey(*pkey, PKEY_PropList_PreviewDetails))
        {
            // This proplist indicates what properties are shown in the details pane at the bottom of the explorer browser.
            pv->vt = VT_BSTR;
            pv->bstrVal = SysAllocString(L"prop:Remote.Permissions;Remote.Owner;Remote.Group;Remote.Size;Remote.Modified");
            hr = pv->bstrVal ? S_OK : E_OUTOFMEMORY;
        }
        else
        {
            hr = _GetColumnDisplayName(pidl, pkey, pv, NULL, 0);
        }
    }
    return hr;
}

//  Retrieves detailed information, identified by a
//  column index, on an item in a Shell folder.
HRESULT CFolderViewImplFolder::GetDetailsOf(PCUITEMID_CHILD pidl,
                                            UINT iColumn,
                                            SHELLDETAILS *pDetails)
{
    PROPERTYKEY key;
    HRESULT hr = MapColumnToSCID(iColumn, &key);
    pDetails->cxChar = 24;
    WCHAR szRet[MAX_PATH];

    if (!pidl)
    {
        // No item means we're returning information about the column itself.
        switch (iColumn)
        {
        case 0:
            pDetails->fmt = LVCFMT_LEFT;
            hr = StringCchCopy(szRet, ARRAYSIZE(szRet), L"Name");
            break;
        case 1:
            pDetails->fmt = LVCFMT_LEFT;
            hr = StringCchCopy(szRet, ARRAYSIZE(szRet), L"Permissions");
            break;
        case 2:
            pDetails->fmt = LVCFMT_LEFT;
            hr = StringCchCopy(szRet, ARRAYSIZE(szRet), L"Owner");
            break;
        case 3:
            pDetails->fmt = LVCFMT_LEFT;
            hr = StringCchCopy(szRet, ARRAYSIZE(szRet), L"Group");
            break;
        case 4:
            pDetails->fmt = LVCFMT_RIGHT;
            hr = StringCchCopy(szRet, ARRAYSIZE(szRet), L"Size");
            break;
        case 5:
            pDetails->fmt = LVCFMT_LEFT;
            hr = StringCchCopy(szRet, ARRAYSIZE(szRet), L"Modified");
            break;
        default:
            // GetDetailsOf is called with increasing column indices until failure.
            hr = E_FAIL;
            break;
        }
    }
    else if (SUCCEEDED(hr))
    {
        hr = _GetColumnDisplayName(pidl, &key, NULL, szRet, ARRAYSIZE(szRet));
    }

    if (SUCCEEDED(hr))
    {
        hr = StringToStrRet(szRet, &pDetails->str);
    }

    return hr;
}

//  Converts a column name to the appropriate
//  property set ID (FMTID) and property ID (PID).
HRESULT CFolderViewImplFolder::MapColumnToSCID(UINT iColumn, PROPERTYKEY *pkey)
{
    HRESULT hr = S_OK;
    switch (iColumn)
    {
    case 0:  *pkey = PKEY_ItemNameDisplay; break;
    case 1:  *pkey = PKEY_Remote_Permissions; break;
    case 2:  *pkey = PKEY_Remote_Owner; break;
    case 3:  *pkey = PKEY_Remote_Group; break;
    case 4:  *pkey = PKEY_Remote_Size; break;
    case 5:  *pkey = PKEY_Remote_Modified; break;
    default: hr = E_FAIL; break;
    }
    return hr;
}

//IPersistFolder2 methods
//  Retrieves the PIDLIST_ABSOLUTE for the folder object.
HRESULT CFolderViewImplFolder::GetCurFolder(PIDLIST_ABSOLUTE *ppidl)
{
    *ppidl = NULL;
    HRESULT hr = m_pidl ? S_OK : E_FAIL;
    if (SUCCEEDED(hr))
    {
        *ppidl = ILCloneFull(m_pidl);
        hr = *ppidl ? S_OK : E_OUTOFMEMORY;
    }
    return hr;
}

// Item idlists passed to folder methods are guaranteed to have accessible memory as specified
// by the cbSize in the itemid.  However they may be loaded from a persisted form (for example
// shortcuts on disk) where they could be corrupted.  It is the shell folder's responsibility
// to make sure it's safe against corrupted or malicious itemids.
PCFVITEMID CFolderViewImplFolder::_IsValid(PCUIDLIST_RELATIVE pidl)
{
    PCFVITEMID pidmine = NULL;
    if (pidl)
    {
        pidmine = (PCFVITEMID)pidl;
        if (!(pidmine->cb && MYOBJID == pidmine->MyObjID && pidmine->nLevel <= g_nMaxLevel))
        {
            pidmine = NULL;
        }
    }
    return pidmine;
}

HRESULT CFolderViewImplFolder::_GetName(PCUIDLIST_RELATIVE pidl, PWSTR pszName, int cchMax)
{
    PCFVITEMID pMyObj = _IsValid(pidl);
    HRESULT hr = pMyObj ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
    {
        // StringCchCopy requires aligned strings, and itemids are not necessarily aligned.
        int i = 0;
        for ( ; i < cchMax; i++)
        {
            pszName[i] = pMyObj->szName[i];
            if (0 == pszName[i])
            {
                break;
            }
        }

        // Make sure the string is null-terminated.
        if (i == cchMax)
        {
            pszName[cchMax - 1] = 0;
        }
    }
    return hr;
}

HRESULT CFolderViewImplFolder::_GetName(PCUIDLIST_RELATIVE pidl, PWSTR *ppsz)
{
    *ppsz = 0;
    PCFVITEMID pMyObj = _IsValid(pidl);
    HRESULT hr = pMyObj ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
    {
        int const cch = pMyObj->cchName;
        *ppsz = (PWSTR)CoTaskMemAlloc(cch * sizeof(**ppsz));
        hr = *ppsz ? S_OK : E_OUTOFMEMORY;
        if (SUCCEEDED(hr))
        {
            hr = _GetName(pidl, *ppsz, cch);
        }
    }
    return hr;
}


HRESULT CFolderViewImplFolder::_GetSides(PCUIDLIST_RELATIVE pidl, int *pSides)
{
    PCFVITEMID pMyObj = _IsValid(pidl);
    HRESULT hr = pMyObj ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
    {
        *pSides = pMyObj->nSides;
    }
    return hr;
}

HRESULT CFolderViewImplFolder::_GetLevel(PCUIDLIST_RELATIVE pidl, int *pLevel)
{
    PCFVITEMID pMyObj = _IsValid(pidl);
    HRESULT hr = pMyObj ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
    {
        *pLevel = pMyObj->nLevel;
    }
    else  // If this fails we are at level zero.
    {
        *pLevel = 0;
    }
    return hr;
}

HRESULT CFolderViewImplFolder::_GetSize(PCUIDLIST_RELATIVE pidl, int *pSize)
{
    PCFVITEMID pMyObj = _IsValid(pidl);
    HRESULT hr = pMyObj ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
    {
        *pSize = pMyObj->nSize;
    }
    return hr;
}

HRESULT CFolderViewImplFolder::_GetFolderness(PCUIDLIST_RELATIVE pidl, BOOL *pfIsFolder)
{
    PCFVITEMID pMyObj = _IsValid(pidl);
    HRESULT hr = pMyObj ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
    {
        *pfIsFolder = pMyObj->fIsFolder;
    }
    return hr;
}

HRESULT CFolderViewImplFolder::_ValidatePidl(PCUIDLIST_RELATIVE pidl)
{
    PCFVITEMID pMyObj = _IsValid(pidl);
    return pMyObj ? S_OK : E_INVALIDARG;
}

HRESULT CFolderViewImplFolder::CreateChildID(PCWSTR pszName, int nLevel, int nSize, int nSides, BOOL fIsFolder, PITEMID_CHILD *ppidl)
{
    // Sizeof an object plus the next cb plus the characters in the string.
    UINT nIDSize = sizeof(FVITEMID) +
                   sizeof(USHORT) +
                   (lstrlen(pszName) * sizeof(WCHAR)) +
                   sizeof(WCHAR);

    // Allocate and zero the memory.
    FVITEMID *lpMyObj = (FVITEMID *)CoTaskMemAlloc(nIDSize);

    HRESULT hr = lpMyObj ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(hr))
    {
        ZeroMemory(lpMyObj, nIDSize);
        lpMyObj->cb = static_cast<short>(nIDSize - sizeof(lpMyObj->cb));
        lpMyObj->MyObjID    = MYOBJID;
        lpMyObj->cchName    = (BYTE)(lstrlen(pszName) + 1);
        lpMyObj->nLevel     = (BYTE)nLevel;
        lpMyObj->nSize      = (BYTE)nSize;
        lpMyObj->nSides     = (BYTE)nSides;
        lpMyObj->fIsFolder  = (BOOL)fIsFolder;

        hr = StringCchCopy(lpMyObj->szName, lpMyObj->cchName, pszName);
        if (SUCCEEDED(hr))
        {
            *ppidl = (PITEMID_CHILD)lpMyObj;
        }
    }
    return hr;
}




CFolderViewImplEnumIDList::CFolderViewImplEnumIDList(DWORD grfFlags, int nLevel, PCWSTR pszPath, CFolderViewImplFolder *pFolderViewImplShellFolder) :
    m_cRef(1), m_grfFlags(grfFlags), m_nLevel(nLevel), m_nItem(0), m_pFolder(pFolderViewImplShellFolder)
{
    m_pFolder->AddRef();
    StringCchCopy(m_szPath, ARRAYSIZE(m_szPath), pszPath ? pszPath : L"/");
}

CFolderViewImplEnumIDList::~CFolderViewImplEnumIDList()
{
    m_pFolder->Release();
}

HRESULT CFolderViewImplEnumIDList::QueryInterface(REFIID riid, void **ppv)
{
    static const QITAB qit[] = {
        QITABENT (CFolderViewImplEnumIDList, IEnumIDList),
        { 0 },
    };
    return QISearch(this, qit, riid, ppv);
}

ULONG CFolderViewImplEnumIDList::AddRef()
{
    return InterlockedIncrement(&m_cRef);
}

ULONG CFolderViewImplEnumIDList::Release()
{
    long cRef = InterlockedDecrement(&m_cRef);
    if (0 == cRef)
    {
        delete this;
    }
    return cRef;
}

// This initializes the enumerator.  In this case we set up a default array of items, this represents our
// data source.  In a real-world implementation the array would be replaced and would be backed by some
// other data source that you traverse and convert into IShellFolder item IDs.
HRESULT CFolderViewImplEnumIDList::Initialize()
{
    ZeroMemory(m_aData, sizeof(m_aData));
    RunFtpList(m_szPath, m_aData, ARRAYSIZE(m_aData));
    return S_OK;
}

// Retrieves the specified number of item identifiers in
// the enumeration sequence and advances the current position
// by the number of items retrieved.
HRESULT CFolderViewImplEnumIDList::Next(ULONG celt, PITEMID_CHILD *rgelt, ULONG *pceltFetched)
{
    ULONG celtFetched = 0;

    HRESULT hr = (pceltFetched || celt <= 1) ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
    {
        ULONG i = 0;
        while (SUCCEEDED(hr) && i < celt && m_nItem < ARRAYSIZE(m_aData) && m_aData[m_nItem].szName[0])
        {
            BOOL fSkip = FALSE;
            if (!(m_grfFlags & SHCONTF_STORAGE))
            {
                if (m_aData[m_nItem].fIsFolder)
                {
                    if (!(m_grfFlags & SHCONTF_FOLDERS))
                    {
                        // this is a folder, but caller doesnt want folders
                        fSkip = TRUE;
                    }
                }
                else
                {
                    if (!(m_grfFlags & SHCONTF_NONFOLDERS))
                    {
                        // this is a file, but caller doesnt want files
                        fSkip = TRUE;
                    }
                }
            }

            if (!fSkip)
            {
                hr = m_pFolder->CreateChildID(m_aData[m_nItem].szName, m_nLevel, 1, 3, m_aData[m_nItem].fIsFolder, &rgelt[i]);
                if (SUCCEEDED(hr))
                {
                    celtFetched++;
                    i++;
                }
            }

            m_nItem++;
        }
    }

    if (pceltFetched)
    {
        *pceltFetched = celtFetched;
    }

    return (celtFetched == celt) ? S_OK : S_FALSE;
}

HRESULT CFolderViewImplEnumIDList::Skip(DWORD celt)
{
    m_nItem += celt;
    return S_OK;
}

HRESULT CFolderViewImplEnumIDList::Reset()
{
    m_nItem = 0;
    return S_OK;
}

HRESULT CFolderViewImplEnumIDList::Clone(IEnumIDList **ppenum)
{
    // this method is rarely used and it's acceptable to not implement it.
    *ppenum = NULL;
    return E_NOTIMPL;
}


class CFolderViewCB : public IShellFolderViewCB,
                      public IFolderViewSettings
{
public:
    CFolderViewCB() : _cRef(1) { }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void **ppv)
    {
        static const QITAB qit[] =
        {
            QITABENT(CFolderViewCB, IShellFolderViewCB),
            QITABENT(CFolderViewCB, IFolderViewSettings),
            { 0 },
        };
        return QISearch(this, qit, riid, ppv);
    }

    IFACEMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&_cRef); }
    IFACEMETHODIMP_(ULONG) Release()
    {
        long cRef = InterlockedDecrement(&_cRef);
        if (0 == cRef)
        {
            delete this;
        }
        return cRef;
    }

    // IShellFolderViewCB
    IFACEMETHODIMP MessageSFVCB(UINT /* uMsg */, WPARAM /* wParam */, LPARAM /* lParam */)
        { return E_NOTIMPL; }

    // IFolderViewSettings
    IFACEMETHODIMP GetColumnPropertyList(REFIID /* riid */, void **ppv)
        { *ppv = NULL; return E_NOTIMPL; }
    IFACEMETHODIMP GetGroupByProperty(PROPERTYKEY * /* pkey */, BOOL * /* pfGroupAscending */)
        { return E_NOTIMPL; }
    IFACEMETHODIMP GetViewMode(FOLDERLOGICALVIEWMODE * /* plvm */)
        { return E_NOTIMPL; }
    IFACEMETHODIMP GetIconSize(UINT * /* puIconSize */)
        { return E_NOTIMPL; }

    IFACEMETHODIMP GetFolderFlags(FOLDERFLAGS *pfolderMask, FOLDERFLAGS *pfolderFlags);

    IFACEMETHODIMP GetSortColumns(SORTCOLUMN * /* rgSortColumns */, UINT /* cColumnsIn */, UINT * /* pcColumnsOut */)
        { return E_NOTIMPL; }
    IFACEMETHODIMP GetGroupSubsetCount(UINT * /* pcVisibleRows */)
        { return E_NOTIMPL; }

private:
    ~CFolderViewCB() { };
    long _cRef;
};

// IFolderViewSettings
IFACEMETHODIMP CFolderViewCB::GetFolderFlags(FOLDERFLAGS *pfolderMask, FOLDERFLAGS *pfolderFlags)
{
    if (pfolderMask)
    {
        *pfolderMask = FWF_USESEARCHFOLDER;
    }

    if (pfolderFlags)
    {
        *pfolderFlags = FWF_USESEARCHFOLDER;
    }

    return S_OK;
}

HRESULT CFolderViewCB_CreateInstance(REFIID riid, void **ppv)
{
    *ppv = NULL;

    HRESULT hr = E_OUTOFMEMORY;
    CFolderViewCB *pfvcb = new (std::nothrow) CFolderViewCB();
    if (pfvcb)
    {
        hr = pfvcb->QueryInterface(riid, ppv);
        pfvcb->Release();
    }
    return hr;
}
