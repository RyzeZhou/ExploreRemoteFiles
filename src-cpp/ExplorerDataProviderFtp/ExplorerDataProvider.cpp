/**************************************************************************
    THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
   ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
   THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
   PARTICULAR PURPOSE.

   (c) Microsoft Corporation. All Rights Reserved.
**************************************************************************/

#include <windows.h>
#include <shlobj.h>
#include <shobjidl_core.h>
#include <propkey.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <shellapi.h>
#include <new>  // std::nothrow
#include <string>
#include <vector>
#include <time.h>
#include "FtpMeta.h"
#include "FtpSites.h"

#include "resource.h"
#include "Utils.h"
#include "ProbeLog.h"
#include "Category.h"
#include "Guid.h"
#include "fvcommands.h"

// background context menu wrapper (defined in ContextMenu.cpp)
HRESULT CFolderViewImplBgMenu_Create(IContextMenu *pDef, PCIDLIST_ABSOLUTE pidlFolder, int level, REFIID riid, void **ppv);

// CF_HDROP upload pipeline (defined in ContextMenu.cpp) — shared by the
// background-menu paste and the folder IDropTarget so both entries behave
// identically (single implementation per function).
void PasteDataObjectToFolder(HWND hwnd, PCWSTR site, PCWSTR folder, IDataObject *pdo, PIDLIST_ABSOLUTE notifyPidl);


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
                              public IPersistFolder2,
                              public IExplorerPaneVisibility
{
public:
    CFolderViewImplFolder(UINT nLevel, PCWSTR pszSite, PCWSTR pszRemotePath);

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

    // IExplorerPaneVisibility (Ribbon opt-in, see MSDN "Extending the Ribbon")
    IFACEMETHODIMP GetPaneState(REFEXPLORERPANE ep, EXPLORERPANESTATE *pps);

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
    WCHAR               m_szSiteName[64];   // empty only at level 0 (site picker)
};

typedef struct
{
    int     nLevel;
    DWORD   dwMode;       // unix permission bits
    ULONGLONG dwSize;    // bytes
    DWORD   dwMtime;      // unix epoch seconds
    DWORD   dwUid;        // 0 = unknown (FTP)
    DWORD   dwGid;
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
static BOOL RunFtpList(PCWSTR site, PCWSTR path, ITEMDATA *out, int maxItems)
{
    FTPENTRY entries[MAX_OBJS] = {};
    int n = FtpListCached(site, path, entries, maxItems > MAX_OBJS ? MAX_OBJS : maxItems);
    for (int i = 0; i < n; i++)
    {
        ITEMDATA &item = out[i];
        item.nLevel = 0;
        item.dwMode  = entries[i].dwMode;
        item.dwMtime = entries[i].dwMtime;
        item.dwSize  = entries[i].dwSize;
        StringCchCopy(item.szOwner, ARRAYSIZE(item.szOwner), entries[i].szOwner);
        StringCchCopy(item.szGroup, ARRAYSIZE(item.szGroup), entries[i].szGroup);
        item.fIsFolder  = entries[i].fIsFolder;
        item.fIsSymlink = entries[i].fIsSymlink;
        StringCchCopy(item.szName, ARRAYSIZE(item.szName), entries[i].szName);
    }
    return n > 0 ? n : 0;
}

static int RunFtpOperation(PCWSTR site, PCWSTR verb, PCWSTR path1, PCWSTR path2)
{
    WCHAR cmd[2400];
    HRESULT hr = path2 && path2[0]
        ? StringCchPrintf(cmd, ARRAYSIZE(cmd), L"\"%s\" %s \"%s\" \"%s\" \"%s\"", GetCliPath(), verb, site, path1, path2)
        : StringCchPrintf(cmd, ARRAYSIZE(cmd), L"\"%s\" %s \"%s\" \"%s\"", GetCliPath(), verb, site, path1);
    if (FAILED(hr)) return -1;
    ProbeLog(L"[FTP] operation cmd='%s'", cmd);
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (!ok)
    {
        ProbeLog(L"[FTP] operation CreateProcess failed err=%lu", GetLastError());
        return -1;
    }
    DWORD wait = WaitForSingleObject(pi.hProcess, 30000);
    if (wait == WAIT_TIMEOUT)
    {
        ProbeLog(L"[FTP] operation timed out; terminating process");
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    ProbeLog(L"[FTP] op=%s path1='%s' path2='%s' wait=%lu exit=%u", verb, path1, path2 ? path2 : L"", wait, code);
    return code == 0 ? 0 : -1;
}
static BOOL GetItemMeta(PCWSTR site, PCWSTR path, PCWSTR name, ITEMDATA *out)
{
    if (!name || !name[0]) return FALSE;
    ProbeLog(L"[META] GetItemMeta site='%s' path='%s' name='%s'", site ? site : L"", path ? path : L"", name);
    std::vector<FTPENTRY> items;
    if (!FtpListCachedAll(site, path ? path : L"/", items)) return FALSE;
    for (auto const &item : items)
    {
        if (0 != StrCmp(item.szName, name)) continue;
        ZeroMemory(out, sizeof(*out));
        out->dwMode = item.dwMode; out->dwMtime = item.dwMtime; out->dwSize = item.dwSize;
        out->dwUid = item.dwUid; out->dwGid = item.dwGid;
        out->fIsFolder = item.fIsFolder; out->fIsSymlink = item.fIsSymlink;
        StringCchCopy(out->szOwner, ARRAYSIZE(out->szOwner), item.szOwner);
        StringCchCopy(out->szGroup, ARRAYSIZE(out->szGroup), item.szGroup);
        StringCchCopy(out->szName, ARRAYSIZE(out->szName), item.szName);
        return TRUE;
    }
    return FALSE;
}
// ---- server-style addressing helpers (used by GetDisplayNameOf and
// GetUIObjectOf's IPropertyStore branch, so defined before both) ------------

static void FormatMode(DWORD mode, BOOL folder, BOOL symlink, PWSTR out, UINT cch);  // fwd
static BOOL GetFriendlyType(PCWSTR name, BOOL fIsFolder, PWSTR out, UINT cch);        // fwd
static BOOL GetSiteColumnValue(PCWSTR siteName, UINT col, PWSTR out, UINT cch);       // fwd

static PCFVITEMID IsOursItem(PCUIDLIST_RELATIVE p)
{
    return (p && p->mkid.cb >= FIELD_OFFSET(FVITEMID, szName) + sizeof(WCHAR) &&
            ((PCFVITEMID)p)->MyObjID == MYOBJID) ? (PCFVITEMID)p : NULL;
}
static BOOL GetPidlSite(PCIDLIST_ABSOLUTE abs, PWSTR out, UINT cch)
{
    out[0] = 0;
    PCUIDLIST_RELATIVE p = (PCUIDLIST_RELATIVE)abs;
    while (p && p->mkid.cb)
    {
        PCFVITEMID it = IsOursItem(p);
        if (it) { StringCchCopyN(out, cch, it->szName, it->cchName); return out[0] != 0; }
        p = ILNext(p);
    }
    return FALSE;
}
static void GetPidlPath(PCIDLIST_ABSOLUTE abs, PWSTR out, UINT cch)
{
    out[0] = 0;
    PCUIDLIST_RELATIVE p = (PCUIDLIST_RELATIVE)abs;
    BOOL first = TRUE;
    while (p && p->mkid.cb)
    {
        PCFVITEMID it = IsOursItem(p);
        if (it)
        {
            WCHAR name[256];
            StringCchCopyN(name, ARRAYSIZE(name), it->szName, it->cchName);
            if (name[0] && !first) { StringCchCat(out, cch, L"/"); StringCchCat(out, cch, name); }
            first = FALSE;
        }
        p = ILNext(p);
    }
    if (!out[0]) StringCchCopy(out, cch, L"/");

    // PIDLs store path components relative to the configured site root.
    // Shell extensions must pass the actual remote path to the CLI.
    WCHAR site[64] = {};
    if (GetPidlSite(abs, site, ARRAYSIZE(site)))
    {
        const FTPSITE *s = FtpSiteFind(site);
        if (s && s->startPath[0] && StrCmp(s->startPath, L"/") != 0)
        {
            if (StrCmp(out, L"/") == 0)
            {
                StringCchCopy(out, cch, s->startPath);
            }
            else
            {
                WCHAR relative[600] = {}, base[256] = {};
                StringCchCopy(relative, ARRAYSIZE(relative), out);
                StringCchCopy(base, ARRAYSIZE(base), s->startPath);
                while (lstrlen(base) > 1 && base[lstrlen(base) - 1] == L'/')
                    base[lstrlen(base) - 1] = L'\0';
                StringCchPrintf(out, cch, L"%s%s", base, relative);
            }
        }
    }
}

// Per-item IPropertyStore so the standard Properties dialog will open for our
// items (and then load the registered property sheet handler with the
// Permissions page). Read-only: values come from the cached directory snapshot.
class CFolderViewImplPropStore : public IPropertyStore
{
public:
    CFolderViewImplPropStore(PCWSTR site, PCWSTR folder, PCWSTR name) : ref(1)
    {
        StringCchCopy(szSite, ARRAYSIZE(szSite), site);
        StringCchCopy(szFolder, ARRAYSIZE(szFolder), folder);
        StringCchCopy(szName, ARRAYSIZE(szName), name);
        GetItemMeta(site, folder, name, &meta);
    }
    HRESULT QueryInterface(REFIID r, void **p)
    {
        static const QITAB q[] = { QITABENT(CFolderViewImplPropStore, IPropertyStore), {0} };
        return QISearch(this, q, r, p);
    }
    ULONG AddRef() { return InterlockedIncrement(&ref); }
    ULONG Release() { long n = InterlockedDecrement(&ref); if (!n) delete this; return n; }

    // IPropertyStore
    HRESULT GetCount(DWORD *pcProps) { if (pcProps) *pcProps = 0; return S_OK; }
    HRESULT GetAt(DWORD, PROPERTYKEY *) { return E_NOTIMPL; }
    HRESULT GetValue(REFPROPERTYKEY key, PROPVARIANT *pv)
    {
        PropVariantInit(pv);
        // Site-picker item (no site segment): expose connection properties.
        if (!szSite[0])
        {
            if (IsEqualPropertyKey(key, PKEY_ItemNameDisplay) || IsEqualPropertyKey(key, PKEY_FileName))
            {
                pv->vt = VT_LPWSTR;
                return SHStrDup(szName, &pv->pwszVal);
            }
            if (IsEqualPropertyKey(key, PKEY_ItemType))
            {
                pv->vt = VT_LPWSTR;
                return SHStrDup(ExplorerText(L"type.connection", L"FTP 连接", L"FTP connection"), &pv->pwszVal);
            }
            const FTPSITE *s = FtpSiteFind(szName);
            if (s)
            {
                if (IsEqualPropertyKey(key, PKEY_Remote_SiteHost))  return SHStrDup(s->host, &pv->pwszVal);
                if (IsEqualPropertyKey(key, PKEY_Remote_SiteProto)) return SHStrDup(s->type, &pv->pwszVal);
                if (IsEqualPropertyKey(key, PKEY_Remote_SitePort))
                {
                    WCHAR buf[16];
                    StringCchPrintf(buf, ARRAYSIZE(buf), L"%d", s->port);
                    pv->vt = VT_LPWSTR;
                    return SHStrDup(buf, &pv->pwszVal);
                }
                if (IsEqualPropertyKey(key, PKEY_Remote_SiteUser))  return SHStrDup(s->user, &pv->pwszVal);
                if (IsEqualPropertyKey(key, PKEY_Remote_SiteStart)) return SHStrDup(s->startPath, &pv->pwszVal);
            }
            return S_OK;
        }
        if (IsEqualPropertyKey(key, PKEY_ItemNameDisplay) || IsEqualPropertyKey(key, PKEY_FileName))
        {
            pv->vt = VT_LPWSTR;
            return SHStrDup(szName, &pv->pwszVal);
        }
        if (IsEqualPropertyKey(key, PKEY_ItemType))
        {
            pv->vt = VT_LPWSTR;
            return SHStrDup(meta.fIsSymlink ? ExplorerText(L"type.symbolic_link", L"符号链接", L"Symbolic Link") : (meta.fIsFolder ? ExplorerText(L"type.folder", L"文件夹", L"Folder") : ExplorerText(L"type.file", L"文件", L"File")), &pv->pwszVal);
        }
        if (IsEqualPropertyKey(key, PKEY_Size))
        {
            pv->vt = VT_UI8;
            pv->uhVal.QuadPart = meta.dwSize;
            return S_OK;
        }
        if (IsEqualPropertyKey(key, PKEY_DateModified))
        {
            pv->vt = VT_FILETIME;
            ULONGLONG ft = ((ULONGLONG)meta.dwMtime) * 10000000ULL + 116444736000000000ULL;
            pv->filetime.dwLowDateTime = (DWORD)(ft & 0xFFFFFFFF);
            pv->filetime.dwHighDateTime = (DWORD)(ft >> 32);
            return S_OK;
        }
        if (IsEqualPropertyKey(key, PKEY_Remote_Permissions))
        {
            pv->vt = VT_LPWSTR;
            WCHAR mode[16];
            FormatMode(meta.dwMode, meta.fIsFolder, meta.fIsSymlink, mode, ARRAYSIZE(mode));
            return SHStrDup(mode, &pv->pwszVal);
        }
        if (IsEqualPropertyKey(key, PKEY_Remote_Type))
        {
            pv->vt = VT_LPWSTR;
            WCHAR type[128];
            GetFriendlyType(szName, meta.fIsFolder, type, ARRAYSIZE(type));
            return SHStrDup(type, &pv->pwszVal);
        }
        if (IsEqualPropertyKey(key, PKEY_Remote_OwnerUid))
        {
            pv->vt = VT_LPWSTR;
            WCHAR buf[16] = {};
            if (meta.dwUid != 0xFFFFFFFF) StringCchPrintf(buf, ARRAYSIZE(buf), L"%u", meta.dwUid);
            return SHStrDup(buf, &pv->pwszVal);
        }
        if (IsEqualPropertyKey(key, PKEY_Remote_GroupGid))
        {
            pv->vt = VT_LPWSTR;
            WCHAR buf[16] = {};
            if (meta.dwGid != 0xFFFFFFFF) StringCchPrintf(buf, ARRAYSIZE(buf), L"%u", meta.dwGid);
            return SHStrDup(buf, &pv->pwszVal);
        }
        return S_OK;
    }
    HRESULT SetValue(REFPROPERTYKEY, const PROPVARIANT &) { return STG_E_ACCESSDENIED; }
    HRESULT Commit() { return E_NOTIMPL; }

private:
    ~CFolderViewImplPropStore() { }
    long ref;
    WCHAR szSite[64];
    WCHAR szFolder[512];
    WCHAR szName[MAX_PATH];
    ITEMDATA meta;
};

static void FormatMode(DWORD mode, BOOL folder, BOOL symlink, PWSTR out, UINT cch)
{
    WCHAR type = symlink ? L'l' : (folder ? L'd' : L'-');
    StringCchPrintf(out, cch, L"%c%c%c%c%c%c%c%c%c%c", type,
        (mode & 0400) ? L'r' : L'-', (mode & 0200) ? L'w' : L'-', (mode & 0100) ? L'x' : L'-',
        (mode & 0040) ? L'r' : L'-', (mode & 0020) ? L'w' : L'-', (mode & 0010) ? L'x' : L'-',
        (mode & 0004) ? L'r' : L'-', (mode & 0002) ? L'w' : L'-', (mode & 0001) ? L'x' : L'-');
}
static void FormatExactBytes(ULONGLONG value, PWSTR out, UINT cch)
{
    WCHAR raw[32] = {}; StringCchPrintf(raw, ARRAYSIZE(raw), L"%llu", value);
    UINT digits = lstrlenW(raw), first = digits % 3; if (!first) first = 3;
    std::wstring text(raw, first);
    for (UINT i = first; i < digits; i += 3) { text += L','; text.append(raw + i, 3); }
    StringCchCopyW(out, cch, text.c_str());
}
static void FormatSize(ULONGLONG size, BOOL folder, PWSTR out, UINT cch)
{
    if (folder) { StringCchCopy(out, cch, L"-"); return; }
    if (size < 1024) { StringCchPrintf(out, cch, L"%llu B", size); return; }
    static const WCHAR *units[] = { L"KB", L"MB", L"GB", L"TB", L"PB" };
    double shown = (double)size / 1024.0; int unit = 0;
    while (shown >= 1024.0 && unit < 4) { shown /= 1024.0; ++unit; }
    WCHAR exact[40] = {}; FormatExactBytes(size, exact, ARRAYSIZE(exact));
    StringCchPrintf(out, cch, L"%.1f %s (%s B)", shown, units[unit], exact);
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
    CFolderViewImplEnumIDList(DWORD grfFlags, int nCurrent, PCWSTR pszSite, PCWSTR pszPath, CFolderViewImplFolder *pFolderViewImplShellFolder);

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
    std::vector<ITEMDATA> m_aData;
    WCHAR m_szPath[512];
    WCHAR m_szSite[64];

    CFolderViewImplFolder *m_pFolder;
};

HRESULT CFolderViewImplFolder_CreateInstance(REFIID riid, void **ppv)
{
    *ppv = NULL;
    CFolderViewImplFolder* pFolderViewImplShellFolder = new (std::nothrow) CFolderViewImplFolder(0, L"", L"");
    HRESULT hr = pFolderViewImplShellFolder ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(hr))
    {
        hr = pFolderViewImplShellFolder->QueryInterface(riid, ppv);
        pFolderViewImplShellFolder->Release();
    }
    return hr;
}

CFolderViewImplFolder::CFolderViewImplFolder(UINT nLevel, PCWSTR pszSite, PCWSTR pszRemotePath) : m_cRef(1), m_nLevel(nLevel), m_pidl(NULL)
{
    DllAddRef();
    StringCchCopy(m_szSiteName, ARRAYSIZE(m_szSiteName), pszSite ? pszSite : L"");
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
        QITABENT(CFolderViewImplFolder, IExplorerPaneVisibility),
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

    // Level 0 is the site picker: the first path segment must be a site name.
    if (m_nLevel == 0)
    {
        // Address bar direct entry: "<site>:/unix/path".
        WCHAR *colon = wcschr(component, L':');
        if (colon && colon != component)
        {
            WCHAR site[64];
            StringCchCopyN(site, ARRAYSIZE(site), component, (int)(colon - component));
            const FTPSITE *s = FtpSiteFind(site);
            if (!s) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

            PIDLIST_RELATIVE result = NULL;
            hr = CreateChildID(site, 1, 1, 3, TRUE, &result);
            if (FAILED(hr)) return hr;

            // Split the unix path into segments and bind step by step.
            std::vector<std::wstring> segs;
            std::wstring cur;
            for (PCWSTR c = colon + 1; ; c++)
            {
                if (*c == L'/' || *c == 0) { if (!cur.empty()) segs.push_back(cur); cur.clear(); if (!*c) break; }
                else cur += *c;
            }
            IShellFolder *folder = NULL;
            hr = BindToObject(result, pbc, IID_PPV_ARGS(&folder));
            for (size_t k = 0; SUCCEEDED(hr) && k < segs.size(); k++)
            {
                PIDLIST_RELATIVE child = NULL;
                hr = folder->ParseDisplayName(hwnd, pbc, (PWSTR)segs[k].c_str(), NULL, &child, NULL);
                if (SUCCEEDED(hr))
                {
                    IShellFolder *nextFolder = NULL;
                    hr = folder->BindToObject(child, pbc, IID_PPV_ARGS(&nextFolder));
                    PIDLIST_RELATIVE combined = ILCombine(result, child);
                    ILFree(child);
                    if (!combined) hr = E_OUTOFMEMORY;
                    if (SUCCEEDED(hr)) { ILFree(result); result = combined; }
                    folder->Release();
                    folder = nextFolder;
                }
            }
            if (folder) folder->Release();
            if (SUCCEEDED(hr))
            {
                *ppidl = result;
                if (pchEaten) *pchEaten = (ULONG)lstrlen(pszName);
                if (pdwAttributes) *pdwAttributes = 0;
            }
            else if (result) ILFree(result);
            return hr;
        }

        const FTPSITE *site = FtpSiteFind(component);
        if (!site) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        PIDLIST_RELATIVE current = NULL;
        hr = CreateChildID(component, 1, 1, 3, TRUE, &current);
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

    std::vector<FTPENTRY> items;
    if (!FtpListCachedAll(m_szSiteName, m_szRemotePath, items)) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    for (auto const &item : items)
    {
        if (0 != StrCmp(item.szName, component)) continue;
        PIDLIST_RELATIVE current = NULL;
        hr = CreateChildID(component, m_nLevel + 1, 1, 3, item.fIsFolder, &current);
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
    if (!ppenumIDList) return E_POINTER;
    *ppenumIDList = NULL;
    ProbeLog(L"[ENUM] level=%d site='%s' path='%s' flags=0x%X", m_nLevel, m_szSiteName, m_szRemotePath, grfFlags);

    CFolderViewImplEnumIDList *penum = new (std::nothrow) CFolderViewImplEnumIDList(grfFlags, m_nLevel + 1, m_szSiteName, m_szRemotePath, this);
    HRESULT hr = penum ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(hr))
    {
        hr = penum->Initialize();
        if (SUCCEEDED(hr))
        {
            hr = penum->QueryInterface(IID_PPV_ARGS(ppenumIDList));
        }
        penum->Release();
    }
    return hr;
}


//  Factory for handlers for the specified item.
HRESULT CFolderViewImplFolder::BindToObject(PCUIDLIST_RELATIVE pidl,
                                            IBindCtx *pbc, REFIID riid, void **ppv)
{
    *ppv = NULL;
    // (hot-path probe removed 2026-09-02: fired per navigation, log IO froze Explorer)
    HRESULT hr = _ValidatePidl(pidl);
    if (SUCCEEDED(hr))
    {
        WCHAR name[MAX_PATH];
        hr = _GetName(pidl, name, ARRAYSIZE(name));
        if (SUCCEEDED(hr))
        {
            WCHAR childPath[512];
            CFolderViewImplFolder *child = NULL;
            if (m_nLevel == 0)
            {
                // Site picker: bind to the site root (path = site StartPath).
                const FTPSITE *site = FtpSiteFind(name);
                if (!site) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
                child = new (std::nothrow) CFolderViewImplFolder(1, name, site->startPath);
                hr = child ? S_OK : E_OUTOFMEMORY;
            }
            else
            {
                if (!m_szRemotePath[0] || (m_szRemotePath[0] == L'/' && !m_szRemotePath[1]))
                    StringCchPrintf(childPath, ARRAYSIZE(childPath), L"/%s", name);
                else
                    StringCchPrintf(childPath, ARRAYSIZE(childPath), L"%s/%s", m_szRemotePath, name);
                child = new (std::nothrow) CFolderViewImplFolder(m_nLevel + 1, m_szSiteName, childPath);
                hr = child ? S_OK : E_OUTOFMEMORY;
            }
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
        // Column model matches GetDetailsOf: 0=Name 1=Type 2=Permissions
        // 3=Owner 4=Group 5=Size 6=Modified (level >=1, remote directory),
        // or 0=Name 1=Host 2=Protocol 3=Port 4=User 5=Start Path (level 0,
        // the connection picker — not a remote directory).
        WCHAR name1[MAX_PATH] = {}, name2[MAX_PATH] = {};
        if (FAILED(_GetName(pidl1, name1, ARRAYSIZE(name1))) ||
            FAILED(_GetName(pidl2, name2, ARRAYSIZE(name2))))
        {
            return E_INVALIDARG;
        }

        int cmp = 0;
        if (m_nLevel == 0)
        {
            const FTPSITE *s1 = FtpSiteFind(name1), *s2 = FtpSiteFind(name2);
            switch (lParam & SHCIDS_COLUMNMASK)
            {
            case 0: // Name
                cmp = StrCmpLogicalW(name1, name2);
                break;
            case 1: // Host
                cmp = StrCmpLogicalW(s1 ? s1->host : L"", s2 ? s2->host : L"");
                if (!cmp) cmp = StrCmpLogicalW(name1, name2);
                break;
            case 2: // Protocol
                cmp = StrCmpLogicalW(s1 ? s1->type : L"", s2 ? s2->type : L"");
                if (!cmp) cmp = StrCmpLogicalW(name1, name2);
                break;
            case 3: // Port
            {
                int p1 = s1 ? s1->port : 0, p2 = s2 ? s2->port : 0;
                if (p1 != p2) cmp = (p1 < p2) ? -1 : 1;
                else cmp = StrCmpLogicalW(name1, name2);
                break;
            }
            case 4: // User
                cmp = StrCmpLogicalW(s1 ? s1->user : L"", s2 ? s2->user : L"");
                if (!cmp) cmp = StrCmpLogicalW(name1, name2);
                break;
            case 5: // Start Path
                cmp = StrCmpLogicalW(s1 ? s1->startPath : L"", s2 ? s2->startPath : L"");
                if (!cmp) cmp = StrCmpLogicalW(name1, name2);
                break;
            default:
                cmp = StrCmpLogicalW(name1, name2);
                break;
            }
        }
        else
        {
            // Folders always sort before files (Windows convention, independent of
            // the sort direction Explorer applies).
            ITEMDATA m1 = {}, m2 = {};
            GetItemMeta(m_szSiteName, m_szRemotePath, name1, &m1);
            GetItemMeta(m_szSiteName, m_szRemotePath, name2, &m2);
            if (m1.fIsFolder != m2.fIsFolder)
            {
                return ResultFromShort(m1.fIsFolder ? -1 : 1);
            }

            switch (lParam & SHCIDS_COLUMNMASK)
            {
            case 0: // Name -- natural (number-aware) order.
                cmp = StrCmpLogicalW(name1, name2);
                break;
            case 1: // Type -- friendly type name, tie-break by name.
            {
                WCHAR t1[128] = {}, t2[128] = {};
                GetFriendlyType(name1, m1.fIsFolder, t1, ARRAYSIZE(t1));
                GetFriendlyType(name2, m2.fIsFolder, t2, ARRAYSIZE(t2));
                cmp = StrCmpLogicalW(t1, t2);
                if (!cmp) cmp = StrCmpLogicalW(name1, name2);
                break;
            }
            case 2: // Permissions (Linux mode string).
            {
                WCHAR p1[16] = {}, p2[16] = {};
                FormatMode(m1.dwMode, m1.fIsFolder, m1.fIsSymlink, p1, ARRAYSIZE(p1));
                FormatMode(m2.dwMode, m2.fIsFolder, m2.fIsSymlink, p2, ARRAYSIZE(p2));
                cmp = StrCmpW(p1, p2);
                if (!cmp) cmp = StrCmpLogicalW(name1, name2);
                break;
            }
            case 3: // Owner.
                cmp = StrCmpLogicalW(m1.szOwner, m2.szOwner);
                if (!cmp) cmp = StrCmpLogicalW(name1, name2);
                break;
            case 4: // UID.
                if (m1.dwUid != m2.dwUid)
                    cmp = (m1.dwUid < m2.dwUid) ? -1 : 1;
                else
                    cmp = StrCmpLogicalW(name1, name2);
                break;
            case 5: // Group.
                cmp = StrCmpLogicalW(m1.szGroup, m2.szGroup);
                if (!cmp) cmp = StrCmpLogicalW(name1, name2);
                break;
            case 6: // GID.
                if (m1.dwGid != m2.dwGid)
                    cmp = (m1.dwGid < m2.dwGid) ? -1 : 1;
                else
                    cmp = StrCmpLogicalW(name1, name2);
                break;
            case 7: // Size.
                if (m1.dwSize != m2.dwSize)
                    cmp = (m1.dwSize < m2.dwSize) ? -1 : 1;
                else
                    cmp = StrCmpLogicalW(name1, name2);
                break;
            case 8: // Modified.
                if (m1.dwMtime != m2.dwMtime)
                    cmp = (m1.dwMtime < m2.dwMtime) ? -1 : 1;
                else
                    cmp = StrCmpLogicalW(name1, name2);
                break;
            default:
                cmp = StrCmpLogicalW(name1, name2);
                break;
            }
        }
        hr = ResultFromShort(cmp);
    }

    if (ResultFromShort(0) == hr)
    {
        // Continue on by binding to the next level.
        hr = _ILCompareRelIDs(this, pidl1, pidl2, lParam);
    }
    return hr;
}

// Folder-background drop target (P0-3): powers Ctrl+V and drag-drop uploads.
// It funnels into the SAME upload pipeline as the background-menu paste
// (PasteDataObjectToFolder) — one implementation per function.
class CFolderDropTarget : public IDropTarget
{
public:
    CFolderDropTarget(PCWSTR site, PCWSTR folder, PIDLIST_ABSOLUTE pidl)
        : m_cRef(1), m_site(site), m_folder(folder), m_pidl(pidl ? ILCloneFull(pidl) : NULL), m_fHdrop(false)
    { DllAddRef(); }
    ~CFolderDropTarget() { if (m_pidl) ILFree(m_pidl); DllRelease(); }

    HRESULT QueryInterface(REFIID riid, void **ppv)
    {
        static const QITAB q[] = { QITABENT(CFolderDropTarget, IDropTarget), {0} };
        return QISearch(this, q, riid, ppv);
    }
    ULONG AddRef() { return InterlockedIncrement(&m_cRef); }
    ULONG Release() { long n = InterlockedDecrement(&m_cRef); if (!n) delete this; return n; }

    IFACEMETHODIMP DragEnter(IDataObject *pdo, DWORD, POINTL, DWORD *pdwEffect)
    { m_fHdrop = HasHdrop(pdo); *pdwEffect = m_fHdrop ? DROPEFFECT_COPY : DROPEFFECT_NONE; return S_OK; }
    IFACEMETHODIMP DragOver(DWORD, POINTL, DWORD *pdwEffect)
    { *pdwEffect = m_fHdrop ? DROPEFFECT_COPY : DROPEFFECT_NONE; return S_OK; }
    IFACEMETHODIMP DragLeave() { return S_OK; }
    IFACEMETHODIMP Drop(IDataObject *pdo, DWORD, POINTL, DWORD *pdwEffect)
    {
        PasteDataObjectToFolder(NULL, m_site.c_str(), m_folder.c_str(), pdo, m_pidl);
        *pdwEffect = DROPEFFECT_COPY;
        return S_OK;
    }

private:
    static bool HasHdrop(IDataObject *pdo)
    {
        FORMATETC fmt = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        return pdo && pdo->QueryGetData(&fmt) == S_OK;
    }
    long m_cRef;
    std::wstring m_site, m_folder;
    PIDLIST_ABSOLUTE m_pidl;
    bool m_fHdrop;
};

//  Called by the Shell to create the View Object and return it.
HRESULT CFolderViewImplFolder::CreateViewObject(HWND hwnd, REFIID riid, void **ppv)
{
    *ppv = NULL;
    ProbeLog(L"[SAMPLE] CreateViewObject riid=%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X level=%d", riid.Data1, riid.Data2, riid.Data3, riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3], riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7], m_nLevel);

    HRESULT hr = E_NOINTERFACE;
    if (riid == IID_IDropTarget)
    {
        // P0-3: Ctrl+V and drag-drop paste into the current remote directory,
        // sharing the background-menu upload pipeline. The site picker
        // (level 0) has nothing to paste into.
        if (m_nLevel >= 1)
        {
            CFolderDropTarget *pdt = new (std::nothrow) CFolderDropTarget(m_szSiteName, m_szRemotePath, m_pidl);
            hr = pdt ? S_OK : E_OUTOFMEMORY;
            if (SUCCEEDED(hr))
            {
                hr = pdt->QueryInterface(riid, ppv);
                pdt->Release();
            }
        }
    }
    else if (riid == IID_IShellView)
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
        // Background context menu for the folder itself: wrap the system default
        // menu (View/Sort/Refresh/Paste) and append our folder commands
        // (show hidden files, copy current path, new folder, paste files, custom).
        DEFCONTEXTMENU dcm = { hwnd, NULL, m_pidl, static_cast<IShellFolder2 *>(this), 0, NULL, NULL, 0, NULL };
        IContextMenu *pDef = NULL;
        hr = SHCreateDefaultContextMenu(&dcm, IID_PPV_ARGS(&pDef));
        if (SUCCEEDED(hr))
        {
            hr = CFolderViewImplBgMenu_Create(pDef, m_pidl, m_nLevel, riid, ppv);
            pDef->Release();
        }
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
    if (!rgfInOut || !apidl || cidl == 0) return E_INVALIDARG;

    DWORD requested = *rgfInOut;
    DWORD common = requested;
    for (UINT i = 0; i < cidl; i++)
    {
        BOOL fIsFolder = FALSE;
        HRESULT hr = _GetFolderness(apidl[i], &fIsFolder);
        if (FAILED(hr)) return hr;

        DWORD attrs = SFGAO_CANRENAME | SFGAO_CANDELETE | SFGAO_HASPROPSHEET;
        if (fIsFolder) attrs |= SFGAO_FOLDER | SFGAO_HASSUBFOLDER | SFGAO_BROWSABLE;

        // Unix dotfiles are semantic metadata. Explorer owns both the visibility
        // toggle and the translucent icon through SFGAO_HIDDEN.
        WCHAR name[MAX_PATH] = {};
        hr = _GetName(apidl[i], name, ARRAYSIZE(name));
        if (FAILED(hr)) return hr;
        if (name[0] == L'.') attrs |= SFGAO_HIDDEN;
        common &= attrs;
    }
    *rgfInOut = common;
    return S_OK;
}

//  Retrieves an OLE interface that can be used to carry out
//  actions on the specified file objects or folders.
HRESULT CFolderViewImplFolder::GetUIObjectOf(HWND hwnd, UINT cidl, PCUITEMID_CHILD_ARRAY apidl,
                                             REFIID riid, UINT * /* prgfInOut */, void **ppv)
{
    *ppv = NULL;
    HRESULT hr;

    ProbeLog(L"[DIAG] GetUIObjectOf riid=%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X cidl=%u level=%d",
             riid.Data1, riid.Data2, riid.Data3, riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
             riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7], cidl, m_nLevel);

    if (riid == IID_IContextMenu)
    {
        // Probe (2026-09-02 freeze hunt): does the hang sit inside
        // SHCreateDefaultContextMenu or after it?
        ProbeLog(L"[MENU] GetUIObjectOf IContextMenu cidl=%u level=%d enter", cidl, m_nLevel);
        DEFCONTEXTMENU const dcm = { hwnd, NULL, m_pidl, static_cast<IShellFolder2 *>(this),
                               cidl, apidl, NULL, 0, NULL };
        hr = SHCreateDefaultContextMenu(&dcm, riid, ppv);
        ProbeLog(L"[MENU] GetUIObjectOf IContextMenu exit hr=0x%08X", hr);
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
                if (m_nLevel == 0)
                {
                    // Site picker: saved connections get a server icon so the
                    // "connection manager" semantics are visually distinct from
                    // remote directories (which show a folder icon).
                    SHSTOCKICONINFO sii = { sizeof(sii) };
                    if (SUCCEEDED(SHGetStockIconInfo(SIID_SERVER, SHGSI_ICONLOCATION, &sii)) && sii.szPath[0])
                        hr = pdxi->SetNormalIcon(sii.szPath, sii.iIcon);
                    else
                        hr = pdxi->SetNormalIcon(L"shell32.dll", 15);
                }
                else
                {
                    // This refers to icon indices in shell32.  You can also supply custom icons or
                    // register IExtractImage to support general images.
                    hr = pdxi->SetNormalIcon(L"shell32.dll", fIsFolder ? 4 : 1);
                }
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
    else if (riid == IID_IPropertyStore)
    {
        // Standard Properties dialog needs a per-item IPropertyStore before it
        // will open (and load our property sheet handlers). Provide the remote
        // metadata we already cache; write-back is handled by the property
        // sheet page / our commands.
        if (cidl >= 1)
        {
            WCHAR site[64] = {}, folder[512] = {}, name[MAX_PATH] = {};
            // The folder object already knows the full remote path. The PIDL
            // only contains the path components relative to StartPath.
            StringCchCopy(site, ARRAYSIZE(site), m_szSiteName);
            StringCchCopy(folder, ARRAYSIZE(folder), m_szRemotePath[0] ? m_szRemotePath : L"/");
            // Level 0 (site picker): the item is a saved connection — the
            // folder PIDL has no site segment; build the store from the site
            // name alone (GetValue branches on empty szSite).
            if (SUCCEEDED(_GetName(apidl[0], name, ARRAYSIZE(name))))
            {
                CFolderViewImplPropStore *ps = new (std::nothrow) CFolderViewImplPropStore(site, folder, name);
                hr = ps ? S_OK : E_OUTOFMEMORY;
                if (SUCCEEDED(hr))
                {
                    hr = ps->QueryInterface(riid, ppv);
                    ps->Release();
                }
            }
        }
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
    ProbeLog(L"[NAME] GetDisplayNameOf flags=0x%X", shgdnFlags);
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
            // Server-style address: "<site>:/<unix path>[/<child>]"
            // (used for both the address bar and FORPARSING, keeping input/output symmetric).
            WCHAR site[64] = {}, path[600] = {};
            BOOL hasSite = GetPidlSite(m_pidl, site, ARRAYSIZE(site));
            // The folder object carries the full path including StartPath.
            StringCchCopy(path, ARRAYSIZE(path), m_szRemotePath[0] ? m_szRemotePath : L"/");
            BOOL siteFromPidl = FALSE;   // child PIDL IS the site root itself
            if (!hasSite)
            {
                // Site picker root: the child PIDL itself may BE the site segment
                // (e.g. address bar shows the site root -> want "WSL-FTP:/").
                WCHAR childSite[64];
                if (GetPidlSite((PCIDLIST_ABSOLUTE)pidl, childSite, ARRAYSIZE(childSite)))
                {
                    StringCchCopy(site, ARRAYSIZE(site), childSite);
                    hasSite = TRUE;
                    const FTPSITE *s = FtpSiteFind(site);
                    StringCchCopy(path, ARRAYSIZE(path), (s && s->startPath[0]) ? s->startPath : L"/");
                    siteFromPidl = TRUE;
                }
            }

            WCHAR szName[MAX_PATH] = {};
            hr = _GetName(pidl, szName, ARRAYSIZE(szName));
            if (SUCCEEDED(hr))
            {
                if (hasSite)
                {
                    StringCchPrintf(szDisplayName, ARRAYSIZE(szDisplayName), L"%s:%s", site, path);
                    if (!siteFromPidl && szName[0])
                    {
                        // Append the child name ("WSL-FTP:/file" at the site root,
                        // "WSL-FTP:/tmp/file" in deep folders).
                        BOOL pathIsRoot = (path[0] == L'/' && !path[1]);
                        if (!pathIsRoot) StringCchCat(szDisplayName, ARRAYSIZE(szDisplayName), L"/");
                        StringCchCat(szDisplayName, ARRAYSIZE(szDisplayName), szName);
                    }
                    // siteFromPidl: the site root itself -> keep just "WSL-FTP:/"
                }
                else
                {
                    // Site picker root has no site segment; fall back to the plain name.
                    StringCchCopy(szDisplayName, ARRAYSIZE(szDisplayName), szName);
                }
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
    if (RunFtpOperation(m_szSiteName, L"rename", oldPath, newPath) != 0)
    {
        MessageBoxW(hwnd, ExplorerText(L"error.rename_failed", L"重命名失败。", L"Rename failed."), ExplorerText(L"dialog.remote", L"远程", L"Remote"), MB_OK | MB_ICONERROR);
        return E_FAIL;
    }
    BOOL folder = FALSE; int size = 0;
    _GetFolderness(pidl, &folder); _GetSize(pidl, &size);
    if (ppidlOut) hr = CreateChildID(pszName, m_nLevel + 1, size > 0 ? size : 1, 3, folder, ppidlOut);
    FtpCacheClear();
    // Auto-refresh via the background notifier (safe: FtpCacheClear no longer
    // touches the bridge synchronously; the notification itself runs off the
    // UI thread).
    FtpNotifyUpdateDir(m_pidl);
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
//  Friendly type name for the "Type" column, resolved by Explorer's own
//  association system (same strings the shell shows for local files).
//  Rule: a dotfile with no second dot (.bashrc) has NO extension; otherwise
//  the extension is from the LAST dot (.hidden-test.txt -> ".txt",
//  archive.tar.gz -> ".gz").
static BOOL GetFriendlyType(PCWSTR name, BOOL fIsFolder, PWSTR out, UINT cch)
{
    if (fIsFolder)
    {
        SHFILEINFOW sfi = {};
        if (SHGetFileInfoW(L"x", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                           SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES) && sfi.szTypeName[0])
        {
            StringCchCopy(out, cch, sfi.szTypeName);
            return TRUE;
        }
        StringCchCopy(out, cch, ExplorerText(L"type.folder", L"文件夹", L"Folder"));
        return TRUE;
    }

    const WCHAR *dot = wcsrchr(name, L'.');
    // no dot / dot is the first char (dotfile w/o second dot) / trailing dot -> no extension
    if (!dot || dot == name || !dot[1])
    {
        SHFILEINFOW sfi = {};
        if (SHGetFileInfoW(L"x", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                           SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES) && sfi.szTypeName[0])
        {
            StringCchCopy(out, cch, sfi.szTypeName);
            return TRUE;
        }
        StringCchCopy(out, cch, ExplorerText(L"type.file", L"文件", L"File"));
        return TRUE;
    }

    // Cache ext -> friendly type (GetDetailsOf is called per item per column).
    static struct { WCHAR ext[16]; WCHAR type[64]; } s_cache[32];
    static int s_n = 0;
    for (int i = 0; i < s_n; i++)
        if (!_wcsicmp(s_cache[i].ext, dot))
        {
            StringCchCopy(out, cch, s_cache[i].type);
            return TRUE;
        }

    WCHAR buf[128] = {};
    DWORD cchBuf = ARRAYSIZE(buf);
    HRESULT hr = AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_FRIENDLYDOCNAME, dot, NULL, buf, &cchBuf);
    if (FAILED(hr) || !buf[0])
        StringCchPrintf(buf, ARRAYSIZE(buf), ExplorerText(L"type.extension_file", L"%s 文件", L"%s file"), dot + 1);   // unknown extension

    int slot = (s_n < 32) ? s_n++ : 0;
    StringCchCopyN(s_cache[slot].ext, ARRAYSIZE(s_cache[slot].ext), dot, ARRAYSIZE(s_cache[slot].ext) - 1);
    StringCchCopy(s_cache[slot].type, ARRAYSIZE(s_cache[slot].type), buf);
    StringCchCopy(out, cch, buf);
    return TRUE;
}

HRESULT CFolderViewImplFolder::GetDefaultColumnState(UINT iColumn, SHCOLSTATEF *pcsFlags)
{
    if (m_nLevel == 0)
    {
        if (iColumn >= 6) return E_INVALIDARG;
        *pcsFlags = SHCOLSTATE_ONBYDEFAULT;
        if (iColumn == 3)      *pcsFlags |= SHCOLSTATE_TYPE_INT;   // Port
        else                   *pcsFlags |= SHCOLSTATE_TYPE_STR;
        return S_OK;
    }
    if (iColumn >= 9) return E_INVALIDARG;
    *pcsFlags = SHCOLSTATE_ONBYDEFAULT;
    if (iColumn == 0)          *pcsFlags |= SHCOLSTATE_TYPE_STR;   // Name
    else if (iColumn == 1)     *pcsFlags |= SHCOLSTATE_TYPE_STR;   // Type
    else if (iColumn == 2)     *pcsFlags |= SHCOLSTATE_TYPE_STR;   // Permissions
    else if (iColumn == 3)     *pcsFlags |= SHCOLSTATE_TYPE_STR;   // Owner
    else if (iColumn == 4)     *pcsFlags |= SHCOLSTATE_TYPE_INT;   // UID
    else if (iColumn == 5)     *pcsFlags |= SHCOLSTATE_TYPE_STR;   // Group
    else if (iColumn == 6)     *pcsFlags |= SHCOLSTATE_TYPE_INT;   // GID
    else if (iColumn == 7)     *pcsFlags |= SHCOLSTATE_TYPE_INT;   // Size
    else if (iColumn == 8)     *pcsFlags |= SHCOLSTATE_TYPE_DATE;  // Modified
    return S_OK;
}

//  Column value for a saved site (level-0 site picker). Semantics: a
//  connection manager, NOT a remote directory — columns describe the
//  connection, not file metadata.
static BOOL GetSiteColumnValue(PCWSTR siteName, UINT col, PWSTR out, UINT cch)
{
    const FTPSITE *s = FtpSiteFind(siteName);
    if (!s) return FALSE;
    switch (col)
    {
    case 0: StringCchCopy(out, cch, s->name); break;
    case 1: StringCchCopy(out, cch, s->host); break;
    case 2: StringCchCopy(out, cch, s->type); break;
    case 3: StringCchPrintf(out, cch, L"%d", s->port); break;
    case 4: StringCchCopy(out, cch, s->user); break;
    case 5: StringCchCopy(out, cch, s->startPath); break;
    default: return FALSE;
    }
    return TRUE;
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
    GetItemMeta(m_szSiteName, m_szRemotePath, name, &meta);

    WCHAR szVal[256] = {};
    if (IsEqualPropertyKey(*pkey, PKEY_ItemNameDisplay))
    {
        StringCchCopy(szVal, ARRAYSIZE(szVal), name);
    }
    else if (IsEqualPropertyKey(*pkey, PKEY_Remote_Type))
    {
        GetFriendlyType(name, fIsFolder, szVal, ARRAYSIZE(szVal));
    }
    else if (IsEqualPropertyKey(*pkey, PKEY_Remote_Permissions))
    {
        FormatMode(meta.dwMode, meta.fIsFolder, meta.fIsSymlink, szVal, ARRAYSIZE(szVal));
    }
    else if (IsEqualPropertyKey(*pkey, PKEY_Remote_Owner))
    {
        StringCchCopy(szVal, ARRAYSIZE(szVal), meta.szOwner);
    }
    else if (IsEqualPropertyKey(*pkey, PKEY_Remote_OwnerUid))
    {
        if (meta.dwUid != 0xFFFFFFFF) StringCchPrintf(szVal, ARRAYSIZE(szVal), L"%u", meta.dwUid);
    }
    else if (IsEqualPropertyKey(*pkey, PKEY_Remote_Group))
    {
        StringCchCopy(szVal, ARRAYSIZE(szVal), meta.szGroup);
    }
    else if (IsEqualPropertyKey(*pkey, PKEY_Remote_GroupGid))
    {
        if (meta.dwGid != 0xFFFFFFFF) StringCchPrintf(szVal, ARRAYSIZE(szVal), L"%u", meta.dwGid);
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
        // The FTP root (level 0) is a CONNECTION PICKER, not a remote
        // directory — it gets its own column set describing the connection.
        if (m_nLevel == 0)
        {
            switch (iColumn)
            {
            case 0:
                pDetails->fmt = LVCFMT_LEFT;
                hr = StringCchCopy(szRet, ARRAYSIZE(szRet), ExplorerText(L"column.name",L"名称",L"Name"));
                break;
            case 1:
                pDetails->fmt = LVCFMT_LEFT;
                hr = StringCchCopy(szRet, ARRAYSIZE(szRet), ExplorerText(L"column.host",L"主机",L"Host"));
                break;
            case 2:
                pDetails->fmt = LVCFMT_LEFT;
                hr = StringCchCopy(szRet, ARRAYSIZE(szRet), ExplorerText(L"column.protocol",L"协议",L"Protocol"));
                break;
            case 3:
                pDetails->fmt = LVCFMT_RIGHT;
                hr = StringCchCopy(szRet, ARRAYSIZE(szRet), ExplorerText(L"column.port",L"端口",L"Port"));
                break;
            case 4:
                pDetails->fmt = LVCFMT_LEFT;
                hr = StringCchCopy(szRet, ARRAYSIZE(szRet), ExplorerText(L"column.user",L"用户",L"User"));
                break;
            case 5:
                pDetails->fmt = LVCFMT_LEFT;
                hr = StringCchCopy(szRet, ARRAYSIZE(szRet), ExplorerText(L"column.start_path",L"起始路径",L"Start Path"));
                break;
            default:
                hr = E_FAIL;
                break;
            }
        }
        else
        {
            switch (iColumn)
            {
            case 0:
                pDetails->fmt = LVCFMT_LEFT;
                hr = StringCchCopy(szRet, ARRAYSIZE(szRet), ExplorerText(L"column.name",L"名称",L"Name"));
                break;
            case 1:
                pDetails->fmt = LVCFMT_LEFT;
                hr = StringCchCopy(szRet, ARRAYSIZE(szRet), ExplorerText(L"column.type",L"类型",L"Type"));
                break;
            case 2:
                pDetails->fmt = LVCFMT_LEFT;
                hr = StringCchCopy(szRet, ARRAYSIZE(szRet), ExplorerText(L"column.permissions",L"权限",L"Permissions"));
                break;
            case 3:
                pDetails->fmt = LVCFMT_LEFT;
                hr = StringCchCopy(szRet, ARRAYSIZE(szRet), ExplorerText(L"column.owner",L"所有者",L"Owner"));
                break;
            case 4:
                pDetails->fmt = LVCFMT_RIGHT;
                hr = StringCchCopy(szRet, ARRAYSIZE(szRet), ExplorerText(L"column.uid",L"UID",L"UID"));
                break;
            case 5:
                pDetails->fmt = LVCFMT_LEFT;
                hr = StringCchCopy(szRet, ARRAYSIZE(szRet), ExplorerText(L"column.group",L"组",L"Group"));
                break;
            case 6:
                pDetails->fmt = LVCFMT_RIGHT;
                hr = StringCchCopy(szRet, ARRAYSIZE(szRet), ExplorerText(L"column.gid",L"GID",L"GID"));
                break;
            case 7:
                pDetails->fmt = LVCFMT_RIGHT;
                hr = StringCchCopy(szRet, ARRAYSIZE(szRet), ExplorerText(L"column.size",L"大小",L"Size"));
                break;
            case 8:
                pDetails->fmt = LVCFMT_LEFT;
                hr = StringCchCopy(szRet, ARRAYSIZE(szRet), ExplorerText(L"column.modified",L"修改日期",L"Modified"));
                break;
            default:
                // GetDetailsOf is called with increasing column indices until failure.
                hr = E_FAIL;
                break;
            }
        }
    }
    else if (SUCCEEDED(hr))
    {
        if (m_nLevel == 0)
        {
            WCHAR siteName[MAX_PATH] = {};
            hr = _GetName(pidl, siteName, ARRAYSIZE(siteName));
            if (SUCCEEDED(hr) && GetSiteColumnValue(siteName, iColumn, szRet, ARRAYSIZE(szRet)))
                hr = S_OK;
            else
                hr = E_FAIL;
        }
        else
        {
            hr = _GetColumnDisplayName(pidl, &key, NULL, szRet, ARRAYSIZE(szRet));
        }
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
    if (m_nLevel == 0)
    {
        switch (iColumn)
        {
        case 0:  *pkey = PKEY_ItemNameDisplay; break;
        case 1:  *pkey = PKEY_Remote_SiteHost; break;
        case 2:  *pkey = PKEY_Remote_SiteProto; break;
        case 3:  *pkey = PKEY_Remote_SitePort; break;
        case 4:  *pkey = PKEY_Remote_SiteUser; break;
        case 5:  *pkey = PKEY_Remote_SiteStart; break;
        default: hr = E_FAIL; break;
        }
    }
    else
    {
        switch (iColumn)
        {
        case 0:  *pkey = PKEY_ItemNameDisplay; break;
        case 1:  *pkey = PKEY_Remote_Type; break;
        case 2:  *pkey = PKEY_Remote_Permissions; break;
        case 3:  *pkey = PKEY_Remote_Owner; break;
        case 4:  *pkey = PKEY_Remote_OwnerUid; break;
        case 5:  *pkey = PKEY_Remote_Group; break;
        case 6:  *pkey = PKEY_Remote_GroupGid; break;
        case 7:  *pkey = PKEY_Remote_Size; break;
        case 8:  *pkey = PKEY_Remote_Modified; break;
        default: hr = E_FAIL; break;
        }
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

// Ribbon opt-in: MSDN "Extending the Ribbon" — namespace extensions must
// request EP_Ribbon via IExplorerPaneVisibility::GetPaneState
// (EPS_FORCE | EPS_DEFAULT_ON) or Explorer falls back to the old command bar.
HRESULT CFolderViewImplFolder::GetPaneState(REFEXPLORERPANE ep, EXPLORERPANESTATE *pps)
{
    if (!pps) return E_POINTER;
    *pps = EPS_DEFAULT_OFF;
    if (IsEqualGUID(ep, EP_Ribbon))
    {
        *pps = EPS_FORCE | EPS_DEFAULT_ON;
        return S_OK;
    }
    return E_NOTIMPL;
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
        if (!(pidmine->cb && MYOBJID == pidmine->MyObjID))
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




CFolderViewImplEnumIDList::CFolderViewImplEnumIDList(DWORD grfFlags, int nLevel, PCWSTR pszSite, PCWSTR pszPath, CFolderViewImplFolder *pFolderViewImplShellFolder) :
    m_cRef(1), m_grfFlags(grfFlags), m_nLevel(nLevel), m_nItem(0), m_pFolder(pFolderViewImplShellFolder)
{
    m_pFolder->AddRef();
    StringCchCopy(m_szSite, ARRAYSIZE(m_szSite), pszSite ? pszSite : L"");
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
// Explorer convention: folders first, then files; natural (numeric-aware) name order.
static int __cdecl CmpItems(void const *a, void const *b)
{
    ITEMDATA const *ia = (ITEMDATA const *)a;
    ITEMDATA const *ib = (ITEMDATA const *)b;
    if (!ia->szName[0] && !ib->szName[0]) return 0;
    if (!ia->szName[0]) return 1;    // terminator sorts last
    if (!ib->szName[0]) return -1;
    if (ia->fIsFolder != ib->fIsFolder) return ia->fIsFolder ? -1 : 1;
    return StrCmpLogicalW(ia->szName, ib->szName);
}
static void SortItems(ITEMDATA *a, int maxItems)
{
    int n = 0;
    while (n < maxItems && a[n].szName[0]) n++;
    if (n > 1) qsort(a, n, sizeof(ITEMDATA), CmpItems);
}

HRESULT CFolderViewImplEnumIDList::Initialize()
{
    m_aData.clear();
    if (m_nLevel == 1)
    {
        // Enumerating children of level 0 (the site picker): list configured sites.
        FTPSITE sites[MAX_OBJS] = {};
        int n = FtpSitesGet(sites, ARRAYSIZE(sites));
        m_aData.reserve(n);
        for (int i = 0; i < n; i++)
        {
            ITEMDATA item = {};
            item.nLevel = 1;
            item.fIsFolder = TRUE;
            item.fIsSymlink = FALSE;
            StringCchCopy(item.szName, ARRAYSIZE(item.szName), sites[i].name);
            m_aData.push_back(item);
        }
    }
    else
    {
        std::vector<FTPENTRY> entries;
        if (!FtpListCachedAll(m_szSite, m_szPath, entries)) return E_FAIL;
        m_aData.reserve(entries.size());
        for (auto const &entry : entries)
        {
            ITEMDATA item = {};
            item.nLevel = m_nLevel;
            item.dwMode = entry.dwMode;
            item.dwMtime = entry.dwMtime;
            item.dwSize = entry.dwSize;
            item.dwUid = entry.dwUid;
            item.dwGid = entry.dwGid;
            item.fIsFolder = entry.fIsFolder;
            item.fIsSymlink = entry.fIsSymlink;
            StringCchCopy(item.szOwner, ARRAYSIZE(item.szOwner), entry.szOwner);
            StringCchCopy(item.szGroup, ARRAYSIZE(item.szGroup), entry.szGroup);
            StringCchCopy(item.szName, ARRAYSIZE(item.szName), entry.szName);
            m_aData.push_back(item);
        }
    }
    SortItems(m_aData.data(), (int)m_aData.size());
    return S_OK;
}

// Retrieves the specified number of item identifiers in
// the enumeration sequence and advances the current position
// by the number of items retrieved.
HRESULT CFolderViewImplEnumIDList::Next(ULONG celt, PITEMID_CHILD *rgelt, ULONG *pceltFetched)
{
    ProbeLog(L"[ENUM] Next celt=%u", celt);
    ULONG celtFetched = 0;

    HRESULT hr = (pceltFetched || celt <= 1) ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr))
    {
        ULONG i = 0;
        while (SUCCEEDED(hr) && i < celt && m_nItem < (int)m_aData.size())
        {
            ProbeLog(L"[ENUM] item='%s' folder=%d flags=0x%X", m_aData[m_nItem].szName, m_aData[m_nItem].fIsFolder, m_grfFlags);
            BOOL fSkip = FALSE;
            // Dotfiles remain in the enumeration. Explorer filters them after
            // GetAttributesOf reports SFGAO_HIDDEN for the individual item.
            if (!fSkip)
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
    IFACEMETHODIMP MessageSFVCB(UINT uMsg, WPARAM /* wParam */, LPARAM /* lParam */)
    {
        // SFVM_GETVIEWSTATE (0x23) asks the callback for FOLDERVIEWOPTIONS bits.
        // The value is stable legacy API; newer SDKs trimmed the enum, so the
        // constant is hardcoded here.
        if (uMsg == 0x23)
        {
            // FVO_SHOWEXT (0x80): force file extensions to be shown (Linux
            // semantics — no "hide known extensions" concept). The Ribbon's
            // "file extensions" checkbox renders checked & disabled here.
            return (HRESULT)0x80;
        }
        return E_NOTIMPL;
    }

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

